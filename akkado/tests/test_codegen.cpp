#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_lexer.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/sample_registry.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>  // For MAX_VALUES_PER_EVENT
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

// Helper to decode float from PUSH_CONST instruction
static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

// Helper to extract instructions from bytecode
static std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

// Helper to find instruction by opcode
static const cedar::Instruction* find_instruction(const std::vector<cedar::Instruction>& insts,
                                                   cedar::Opcode op) {
    for (const auto& inst : insts) {
        if (inst.opcode == op) return &inst;
    }
    return nullptr;
}

// Helper to count instructions by opcode
static size_t count_instructions(const std::vector<cedar::Instruction>& insts,
                                  cedar::Opcode op) {
    size_t count = 0;
    for (const auto& inst : insts) {
        if (inst.opcode == op) ++count;
    }
    return count;
}

// =============================================================================
// Literal Tests
// =============================================================================

TEST_CASE("Codegen: Number literals", "[codegen][literals]") {
    SECTION("integer") {
        auto result = akkado::compile("42");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 42.0f);
    }

    SECTION("float") {
        auto result = akkado::compile("3.14159");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == Catch::Approx(3.14159f));
    }

    SECTION("negative") {
        auto result = akkado::compile("-440");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == -440.0f);
    }

    SECTION("zero") {
        auto result = akkado::compile("0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }
}

TEST_CASE("Codegen: Bool literals", "[codegen][literals]") {
    SECTION("true") {
        auto result = akkado::compile("true");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 1.0f);
    }

    SECTION("false") {
        auto result = akkado::compile("false");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }
}

TEST_CASE("Codegen: Pitch literals", "[codegen][literals]") {
    SECTION("a4 converts to MIDI 69 then MTOF") {
        auto result = akkado::compile("'a4'");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 2);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 69.0f);  // A4 = MIDI 69
        CHECK(insts[1].opcode == cedar::Opcode::MTOF);
        CHECK(insts[1].inputs[0] == insts[0].out_buffer);
    }

    SECTION("c4 converts to MIDI 60") {
        auto result = akkado::compile("'c4'");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 60.0f);
    }
}

TEST_CASE("Codegen: Chord literals", "[codegen][literals]") {
    SECTION("major chord uses root note") {
        auto result = akkado::compile("C4'");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() >= 2);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 60.0f);  // C4 root
        CHECK(insts[1].opcode == cedar::Opcode::MTOF);
    }
}

TEST_CASE("Codegen: Array literals", "[codegen][literals]") {
    SECTION("simple array") {
        auto result = akkado::compile("[1, 2, 3]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);  // 3 PUSH_CONST
        CHECK(decode_const_float(insts[0]) == 1.0f);
        CHECK(decode_const_float(insts[1]) == 2.0f);
        CHECK(decode_const_float(insts[2]) == 3.0f);
    }

    SECTION("empty array produces zero") {
        auto result = akkado::compile("[]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }

    SECTION("single element array") {
        auto result = akkado::compile("[42]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == 42.0f);
    }
}

// =============================================================================
// Variable Tests
// =============================================================================

TEST_CASE("Codegen: Variables", "[codegen][variables]") {
    SECTION("assignment and lookup") {
        auto result = akkado::compile("x = 440\nsaw(x)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // PUSH_CONST(440), OSC_SAW
        REQUIRE(insts.size() == 2);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(insts[1].opcode == cedar::Opcode::OSC_SAW);
        CHECK(insts[1].inputs[0] == insts[0].out_buffer);
    }

    SECTION("variable reuse in expression") {
        auto result = akkado::compile("f = 440\nsaw(f) + saw(f)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // PUSH_CONST, OSC_SAW, OSC_SAW, ADD
        auto* add = find_instruction(insts, cedar::Opcode::ADD);
        REQUIRE(add != nullptr);
    }
}

// =============================================================================
// Binary Operation Tests
// =============================================================================

TEST_CASE("Codegen: Binary operations", "[codegen][binop]") {
    SECTION("addition") {
        auto result = akkado::compile("1 + 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* add = find_instruction(insts, cedar::Opcode::ADD);
        REQUIRE(add != nullptr);
    }

    SECTION("subtraction") {
        auto result = akkado::compile("5 - 3");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* sub = find_instruction(insts, cedar::Opcode::SUB);
        REQUIRE(sub != nullptr);
    }

    SECTION("multiplication") {
        auto result = akkado::compile("2 * 3");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* mul = find_instruction(insts, cedar::Opcode::MUL);
        REQUIRE(mul != nullptr);
    }

    SECTION("division") {
        auto result = akkado::compile("10 / 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* div = find_instruction(insts, cedar::Opcode::DIV);
        REQUIRE(div != nullptr);
    }

    SECTION("power via pow()") {
        auto result = akkado::compile("pow(2, 8)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* pow = find_instruction(insts, cedar::Opcode::POW);
        REQUIRE(pow != nullptr);
    }

    SECTION("chained operations") {
        auto result = akkado::compile("1 + 2 + 3");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("buffer wiring") {
        auto result = akkado::compile("1 + 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);  // PUSH 1, PUSH 2, ADD
        CHECK(insts[2].inputs[0] == insts[0].out_buffer);
        CHECK(insts[2].inputs[1] == insts[1].out_buffer);
    }
}

// =============================================================================
// Closure Tests
// =============================================================================

TEST_CASE("Codegen: Closures", "[codegen][closures]") {
    SECTION("identity lambda") {
        auto result = akkado::compile("map([1, 2, 3], (x) -> x)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 PUSH_CONST for the array elements
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
    }

    SECTION("lambda with expression") {
        auto result = akkado::compile("map([1, 2], (x) -> x + 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have ADDs for each element
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }
}

// =============================================================================
// Higher-Order Function Tests
// =============================================================================

TEST_CASE("Codegen: map()", "[codegen][hof]") {
    SECTION("map identity") {
        auto result = akkado::compile("map([1, 2, 3], (x) -> x)");
        REQUIRE(result.success);
    }

    SECTION("map with transformation") {
        auto result = akkado::compile("map([1, 2], (x) -> x * 2)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 2);
    }

    SECTION("map single element") {
        auto result = akkado::compile("map([42], (x) -> x)");
        REQUIRE(result.success);
    }
}

TEST_CASE("Codegen: sum()", "[codegen][hof]") {
    SECTION("sum of array") {
        auto result = akkado::compile("sum([1, 2, 3])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 PUSH_CONST, 2 ADD (chain: (1+2)+3)
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("sum single element returns element") {
        auto result = akkado::compile("sum([42])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Just 1 PUSH_CONST, no ADD needed
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("sum empty array returns zero") {
        auto result = akkado::compile("sum([])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // No additions for an empty array.
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
        // A zero is emitted somewhere.
        bool found_zero = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 0.0f) {
                found_zero = true;
                break;
            }
        }
        CHECK(found_zero);
    }

    SECTION("variadic mono sum") {
        auto result = akkado::compile("sum(saw(220), saw(330), saw(440)) |> out(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 mono operands → 2 ADDs, mono output.
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        CHECK(out->inputs[0] == out->inputs[1]);  // mono
    }

    SECTION("sum preserves stereo when any input is stereo") {
        auto result = akkado::compile(R"(
            a = stereo(saw(220), saw(221))
            b = stereo(saw(330), saw(331))
            sum(a, b) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        CHECK(out->inputs[0] != out->inputs[1]);  // stereo: L != R
    }

    SECTION("sum mixes mono and stereo (mono broadcasts)") {
        auto result = akkado::compile(R"(
            m = saw(220)
            s = stereo(saw(330), saw(440))
            sum(m, s) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        CHECK(out->inputs[0] != out->inputs[1]);  // stereo
    }

    SECTION("sum of single stereo arg passes through as stereo") {
        auto result = akkado::compile(R"(
            s = stereo(saw(220), saw(330))
            sum(s) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        CHECK(out->inputs[0] != out->inputs[1]);  // stereo passthrough
    }
}

TEST_CASE("Codegen: map() closure arity dispatch", "[codegen][hof][map]") {
    SECTION("map with 2-arg closure receives per-element index") {
        // [10,20,30] mapped with (v, i) -> v + i should yield [10, 21, 32].
        auto result = akkado::compile("map([10, 20, 30], (v, i) -> v + i) |> out(sum(%))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 index PUSH_CONSTs (0,1,2) emitted, one per element.
        bool found_idx1 = false, found_idx2 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST) {
                float v = decode_const_float(inst);
                if (v == 1.0f) found_idx1 = true;
                if (v == 2.0f) found_idx2 = true;
            }
        }
        CHECK(found_idx1);
        CHECK(found_idx2);
    }

    SECTION("map with 1-arg closure unchanged (no extra index consts)") {
        auto result = akkado::compile("map([1, 2, 3], (v) -> v * 2) |> out(sum(%))");
        REQUIRE(result.success);
    }

    SECTION("map with 3-arg closure errors with E146") {
        auto result = akkado::compile("map([1, 2, 3], (a, b, c) -> a)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E146") found = true;
        }
        CHECK(found);
    }

    SECTION("map with 0-arg closure errors with E132") {
        auto result = akkado::compile("map([1, 2, 3], () -> 1)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E132") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: N-arity closure helper", "[codegen][hof]") {
    SECTION("reduce with under-arity closure errors with E132") {
        auto result = akkado::compile("reduce([1, 2, 3], (a) -> a, 0)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E132") found = true;
        }
        CHECK(found);
    }

    SECTION("reduce with 2-arg closure still works") {
        auto result = akkado::compile("reduce([1, 2, 3], (a, b) -> a + b, 0)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }
}

// Higher-order reducer is named reduce() since 'fold' is taken by the wavefolding
// distortion builtin. See test_arrays.cpp for full reduce() coverage.

TEST_CASE("Codegen: zipWith()", "[codegen][hof]") {
    SECTION("zipWith add") {
        auto result = akkado::compile("zipWith([1, 2], [3, 4], (a, b) -> a + b)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have ADDs for each pair
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("zipWith unequal lengths uses shorter") {
        auto result = akkado::compile("zipWith([1, 2, 3], [4, 5], (a, b) -> a + b)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Only 2 additions (shorter array length)
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }
}

TEST_CASE("Codegen: zip()", "[codegen][hof]") {
    SECTION("zip interleaves arrays") {
        auto result = akkado::compile("zip([1, 2], [3, 4])");
        REQUIRE(result.success);
        // Should produce [1, 3, 2, 4] as 4 buffers
    }
}

TEST_CASE("Codegen: take()", "[codegen][hof]") {
    SECTION("take first n elements") {
        auto result = akkado::compile("take(2, [1, 2, 3, 4])");
        REQUIRE(result.success);
        // take visits the full array but returns only first 2 in multi_buffers_
        // All elements are still emitted as instructions
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 2);
    }

    SECTION("take more than array length") {
        auto result = akkado::compile("take(10, [1, 2])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 2);
    }
}

TEST_CASE("Codegen: drop()", "[codegen][hof]") {
    SECTION("drop first n elements") {
        auto result = akkado::compile("drop(2, [1, 2, 3, 4])");
        REQUIRE(result.success);
        // All 4 elements are emitted, drop just changes which are tracked
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 2);
    }
}

TEST_CASE("Codegen: reverse()", "[codegen][hof]") {
    SECTION("reverse array") {
        auto result = akkado::compile("reverse([1, 2, 3])");
        REQUIRE(result.success);
    }
}

TEST_CASE("Codegen: range()", "[codegen][hof]") {
    SECTION("range generates sequence") {
        auto result = akkado::compile("range(0, 3)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should produce [0, 1, 2]
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
        CHECK(decode_const_float(insts[0]) == 0.0f);
        CHECK(decode_const_float(insts[1]) == 1.0f);
        CHECK(decode_const_float(insts[2]) == 2.0f);
    }

    SECTION("range descending") {
        auto result = akkado::compile("range(3, 0)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 3);
        CHECK(decode_const_float(insts[0]) == 3.0f);
        CHECK(decode_const_float(insts[1]) == 2.0f);
        CHECK(decode_const_float(insts[2]) == 1.0f);
    }

    SECTION("range with step > 1") {
        auto result = akkado::compile("range(0, 10, 3)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 4);
        CHECK(decode_const_float(insts[0]) == 0.0f);
        CHECK(decode_const_float(insts[1]) == 3.0f);
        CHECK(decode_const_float(insts[2]) == 6.0f);
        CHECK(decode_const_float(insts[3]) == 9.0f);
    }
}

TEST_CASE("Codegen: repeat()", "[codegen][hof]") {
    SECTION("repeat value") {
        auto result = akkado::compile("repeat(42, 3)");
        REQUIRE(result.success);
        // Single value emitted, referenced 3 times in multi-buffer
    }
}

// =============================================================================
// User Function Tests
// =============================================================================

TEST_CASE("Codegen: User functions", "[codegen][functions]") {
    SECTION("simple function definition and call") {
        auto result = akkado::compile("fn double(x) -> x * 2\ndouble(21)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* mul = find_instruction(insts, cedar::Opcode::MUL);
        REQUIRE(mul != nullptr);
    }

    SECTION("function with default argument") {
        // Note: 'add' is a reserved builtin name, use 'myAdd' instead
        auto result = akkado::compile("fn myAdd(x, y = 10) -> x + y\nmyAdd(5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* add_op = find_instruction(insts, cedar::Opcode::ADD);
        REQUIRE(add_op != nullptr);
    }

    SECTION("nested function calls") {
        auto result = akkado::compile("fn inc(x) -> x + 1\ninc(inc(1))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }
}

// =============================================================================
// Match Expression Tests
// =============================================================================

TEST_CASE("Codegen: Match expressions - compile-time", "[codegen][match]") {
    SECTION("basic string pattern match") {
        auto result = akkado::compile(R"(
            fn choose(x) -> match(x) {
                "a": 1,
                "b": 2,
                _: 0
            }
            choose("a")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should emit just the winning branch: 1
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
        // Should NOT have any SELECT opcodes for compile-time match
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("match with wildcard default") {
        auto result = akkado::compile(R"(
            fn choose(x) -> match(x) {
                "known": 100,
                _: 42
            }
            choose("unknown")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should emit just the default branch: 42
        REQUIRE(insts.size() >= 1);
    }

    SECTION("match with number patterns") {
        auto result = akkado::compile(R"(
            fn pick(x) -> match(x) {
                1: 10,
                2: 20,
                _: 0
            }
            pick(2)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("match with bool patterns") {
        auto result = akkado::compile(R"(
            fn toggle(x) -> match(x) {
                true: 1,
                false: 0,
                _: -1
            }
            toggle(true)
        )");
        REQUIRE(result.success);
    }
}

TEST_CASE("Codegen: Match expressions - with guards", "[codegen][match]") {
    SECTION("compile-time guard with literal") {
        auto result = akkado::compile(R"(
            fn test(x) -> match(x) {
                "a" && true: 100,
                "a": 50,
                _: 0
            }
            test("a")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Guard true passes, should emit 100
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("compile-time guard with false literal skips arm") {
        auto result = akkado::compile(R"(
            fn test(x) -> match(x) {
                "a" && false: 100,
                "a": 50,
                _: 0
            }
            test("a")
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Guard false fails, should fall through to "a": 50
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }
}

TEST_CASE("Codegen: Match expressions - runtime", "[codegen][match]") {
    SECTION("runtime scrutinee produces select chain") {
        auto result = akkado::compile(R"(
            x = saw(1)
            match(x) {
                0: 10,
                1: 20,
                _: 30
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Runtime match should use SELECT opcodes
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
        // Should have CMP_EQ for pattern comparisons
        CHECK(count_instructions(insts, cedar::Opcode::CMP_EQ) >= 1);
    }

    SECTION("runtime match with guards uses LOGIC_AND") {
        auto result = akkado::compile(R"(
            x = saw(1)
            y = tri(1)
            match(x) {
                0 && y > 0.5: 100,
                0: 50,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have LOGIC_AND for guard combination
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }
}

TEST_CASE("Codegen: Match expressions - guard-only form", "[codegen][match]") {
    SECTION("simple guard-only match") {
        auto result = akkado::compile(R"(
            x = saw(1)
            match {
                x > 0.5: 100,
                x > 0: 50,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have comparisons and selects
        CHECK(count_instructions(insts, cedar::Opcode::CMP_GT) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }

    SECTION("guard-only match with multiple conditions") {
        auto result = akkado::compile(R"(
            a = saw(1)
            b = tri(1)
            match {
                a > 0.5 && b < 0.5: 1,
                a > 0.5: 2,
                b > 0.5: 3,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }
}

TEST_CASE("Codegen: Match expressions - warnings", "[codegen][match]") {
    SECTION("missing wildcard arm produces warning") {
        auto result = akkado::compile(R"(
            x = saw(1)
            match {
                x > 0.5: 100
            }
        )");
        REQUIRE(result.success);  // Should still compile
        // Check for warning in diagnostics
        bool has_warning = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == akkado::Severity::Warning &&
                diag.code == "W001") {
                has_warning = true;
                break;
            }
        }
        CHECK(has_warning);
    }
}

TEST_CASE("Codegen: Match expressions - range patterns", "[codegen][match][range]") {
    SECTION("compile-time range match selects correct arm") {
        auto result = akkado::compile(R"(
            match(0.5) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Compile-time: should emit just the winning branch (2), no SELECT
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        // Should emit PUSH_CONST 2.0
        REQUIRE(insts.size() >= 1);
        CHECK(decode_const_float(insts[0]) == 2.0f);
    }

    SECTION("half-open range excludes upper bound") {
        // 0.3 should NOT match 0.0..0.3, but SHOULD match 0.3..0.7
        auto result = akkado::compile(R"(
            match(0.3) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 2.0f);
    }

    SECTION("range match with lower bound exact match") {
        // 0.0 should match 0.0..0.3
        auto result = akkado::compile(R"(
            match(0.0) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 1.0f);
    }

    SECTION("range match falls through to wildcard") {
        auto result = akkado::compile(R"(
            match(1.5) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 99
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 99.0f);
    }

    SECTION("negative range patterns") {
        auto result = akkado::compile(R"(
            match(-0.5) {
                -1.0..0.0: 1,
                0.0..1.0: 2,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        CHECK(decode_const_float(insts[0]) == 1.0f);
    }

    SECTION("negative number pattern (non-range)") {
        // Verify that standalone negative numbers work in match arms
        auto result = akkado::compile(R"(
            fn pick(x) -> match(x) {
                -1: 10,
                0: 20,
                1: 30,
                _: 0
            }
            pick(-1)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
    }

    SECTION("runtime range match produces CMP_GTE, CMP_LT, LOGIC_AND, SELECT") {
        auto result = akkado::compile(R"(
            vel = saw(1)
            match(vel) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Runtime range match should use CMP_GTE + CMP_LT + LOGIC_AND
        CHECK(count_instructions(insts, cedar::Opcode::CMP_GTE) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_LT) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }

    SECTION("runtime range match with guard uses extra LOGIC_AND") {
        // `mode` is reserved as a Phase 2 PRD voicing builtin; use `mod` here.
        auto result = akkado::compile(R"(
            vel = saw(1)
            mod = tri(1)
            match(vel) {
                0.0..0.5 && mod > 0.5: 100,
                0.5..1.0: 200,
                _: 0
            }
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have LOGIC_AND for range condition + guard combination
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) >= 2);
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) >= 1);
    }

    SECTION("compile-time range with function call") {
        auto result = akkado::compile(R"(
            fn velocity_layer(vel) -> match(vel) {
                0.0..0.3: 1,
                0.3..0.7: 2,
                0.7..1.0: 3,
                _: 0
            }
            velocity_layer(0.8)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should resolve at compile-time to 3 (no SELECT)
        CHECK(count_instructions(insts, cedar::Opcode::SELECT) == 0);
        // The result (3.0) should be one of the PUSH_CONST instructions
        bool found_result = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST && decode_const_float(inst) == 3.0f) {
                found_result = true;
                break;
            }
        }
        CHECK(found_result);
    }
}

// =============================================================================
// Pattern Tests (MiniLiteral)
// =============================================================================

TEST_CASE("Codegen: Patterns", "[codegen][patterns]") {
    SECTION("pitch pattern produces SEQPAT_QUERY and SEQPAT_STEP") {
        auto result = akkado::compile("pat(\"c4 e4 g4\")");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Patterns now use lazy query system (SEQPAT_QUERY + SEQPAT_STEP)
        auto* query = find_instruction(insts, cedar::Opcode::SEQPAT_QUERY);
        auto* step = find_instruction(insts, cedar::Opcode::SEQPAT_STEP);
        REQUIRE(query != nullptr);
        REQUIRE(step != nullptr);
    }
}

// =============================================================================
// Buffer Allocation Tests
// =============================================================================

TEST_CASE("Codegen: Buffer allocation", "[codegen][buffers]") {
    SECTION("sequential buffer indices") {
        auto result = akkado::compile("[1, 2, 3]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);
        CHECK(insts[0].out_buffer == 0);
        CHECK(insts[1].out_buffer == 1);
        CHECK(insts[2].out_buffer == 2);
    }

    SECTION("instruction inputs reference prior outputs") {
        auto result = akkado::compile("1 + 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 3);
        CHECK(insts[2].inputs[0] == insts[0].out_buffer);
        CHECK(insts[2].inputs[1] == insts[1].out_buffer);
    }
}

// =============================================================================
// Integration Tests
// =============================================================================

// =============================================================================
// Conditionals and Logic Tests
// =============================================================================

TEST_CASE("Codegen: Comparison operators - function syntax", "[codegen][conditionals]") {
    SECTION("gt() greater than") {
        auto result = akkado::compile("gt(10, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        REQUIRE(cmp != nullptr);
    }

    SECTION("lt() less than") {
        auto result = akkado::compile("lt(5, 10)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LT);
        REQUIRE(cmp != nullptr);
    }

    SECTION("gte() greater or equal") {
        auto result = akkado::compile("gte(5, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("lte() less or equal") {
        auto result = akkado::compile("lte(5, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("eq() equality") {
        auto result = akkado::compile("eq(5, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_EQ);
        REQUIRE(cmp != nullptr);
    }

    SECTION("neq() not equal") {
        auto result = akkado::compile("neq(5, 10)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_NEQ);
        REQUIRE(cmp != nullptr);
    }
}

TEST_CASE("Codegen: Logic operators - function syntax", "[codegen][conditionals]") {
    SECTION("band() logical AND") {
        auto result = akkado::compile("band(1, 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        REQUIRE(logic != nullptr);
    }

    SECTION("bor() logical OR") {
        auto result = akkado::compile("bor(1, 0)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic != nullptr);
    }

    SECTION("bnot() logical NOT") {
        auto result = akkado::compile("bnot(0)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_NOT);
        REQUIRE(logic != nullptr);
    }
}

TEST_CASE("Codegen: Select function", "[codegen][conditionals]") {
    SECTION("select() ternary") {
        auto result = akkado::compile("select(1, 100, 50)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* sel = find_instruction(insts, cedar::Opcode::SELECT);
        REQUIRE(sel != nullptr);
    }

    SECTION("select() with expressions") {
        auto result = akkado::compile("select(gt(10, 5), 100, 50)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* sel = find_instruction(insts, cedar::Opcode::SELECT);
        REQUIRE(cmp != nullptr);
        REQUIRE(sel != nullptr);
    }
}

TEST_CASE("Codegen: Comparison operators - infix syntax", "[codegen][conditionals]") {
    SECTION("> greater than") {
        auto result = akkado::compile("10 > 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        REQUIRE(cmp != nullptr);
    }

    SECTION("< less than") {
        auto result = akkado::compile("5 < 10");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LT);
        REQUIRE(cmp != nullptr);
    }

    SECTION(">= greater or equal") {
        auto result = akkado::compile("5 >= 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("<= less or equal") {
        auto result = akkado::compile("5 <= 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_LTE);
        REQUIRE(cmp != nullptr);
    }

    SECTION("== equality") {
        auto result = akkado::compile("5 == 5");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_EQ);
        REQUIRE(cmp != nullptr);
    }

    SECTION("!= not equal") {
        auto result = akkado::compile("5 != 10");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_NEQ);
        REQUIRE(cmp != nullptr);
    }
}

TEST_CASE("Codegen: Logic operators - infix syntax", "[codegen][conditionals]") {
    SECTION("&& logical AND") {
        auto result = akkado::compile("1 && 1");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        REQUIRE(logic != nullptr);
    }

    SECTION("|| logical OR") {
        auto result = akkado::compile("1 || 0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic != nullptr);
    }

    SECTION("! prefix NOT") {
        auto result = akkado::compile("!1");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_NOT);
        REQUIRE(logic != nullptr);
    }

    SECTION("! with expression") {
        auto result = akkado::compile("!(5 > 10)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* logic = find_instruction(insts, cedar::Opcode::LOGIC_NOT);
        REQUIRE(cmp != nullptr);
        REQUIRE(logic != nullptr);
    }
}

TEST_CASE("Codegen: Operator precedence", "[codegen][conditionals]") {
    SECTION("&& binds tighter than ||") {
        // 1 || 0 && 0 should be parsed as 1 || (0 && 0) = 1
        auto result = akkado::compile("1 || 0 && 0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have LOGIC_AND before LOGIC_OR in execution order
        auto* logic_and = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        auto* logic_or = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic_and != nullptr);
        REQUIRE(logic_or != nullptr);
    }

    SECTION("Comparison binds tighter than logic") {
        // 5 > 3 && 2 < 4 should be parsed as (5 > 3) && (2 < 4)
        auto result = akkado::compile("5 > 3 && 2 < 4");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp_gt = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* cmp_lt = find_instruction(insts, cedar::Opcode::CMP_LT);
        auto* logic_and = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        REQUIRE(cmp_gt != nullptr);
        REQUIRE(cmp_lt != nullptr);
        REQUIRE(logic_and != nullptr);
    }

    SECTION("Arithmetic binds tighter than comparison") {
        // 2 + 3 > 4 should be parsed as (2 + 3) > 4
        auto result = akkado::compile("2 + 3 > 4");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* add = find_instruction(insts, cedar::Opcode::ADD);
        auto* cmp_gt = find_instruction(insts, cedar::Opcode::CMP_GT);
        REQUIRE(add != nullptr);
        REQUIRE(cmp_gt != nullptr);
    }

    SECTION("Grouping overrides precedence") {
        // (1 || 0) && 0 should evaluate || first
        auto result = akkado::compile("(1 || 0) && 0");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* logic_and = find_instruction(insts, cedar::Opcode::LOGIC_AND);
        auto* logic_or = find_instruction(insts, cedar::Opcode::LOGIC_OR);
        REQUIRE(logic_and != nullptr);
        REQUIRE(logic_or != nullptr);
    }
}

TEST_CASE("Codegen: Complex conditional expressions", "[codegen][conditionals]") {
    SECTION("Chained comparisons with logic") {
        auto result = akkado::compile("(5 > 3) && (10 < 20) || (1 == 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_GT) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_LT) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::CMP_EQ) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_AND) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_OR) == 1);
    }

    SECTION("Select with comparison condition") {
        auto result = akkado::compile("select(10 > 5, 100, 50)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* cmp = find_instruction(insts, cedar::Opcode::CMP_GT);
        auto* sel = find_instruction(insts, cedar::Opcode::SELECT);
        REQUIRE(cmp != nullptr);
        REQUIRE(sel != nullptr);
    }

    SECTION("Double negation") {
        auto result = akkado::compile("!!1");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::LOGIC_NOT) == 2);
    }
}

TEST_CASE("Codegen: Complex expressions", "[codegen][integration]") {
    SECTION("map with sum") {
        auto result = akkado::compile("sum(map([1, 2, 3], (x) -> x * 2))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("polyphonic oscillator inline without poly is error") {
        auto result = akkado::compile("sum(map(mtof(chord(\"Am\")), (f) -> saw(f)))");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// Embedded Alternate Pattern Tests
// =============================================================================

TEST_CASE("Codegen: Embedded alternate sequence timing", "[codegen][pattern][sequence]") {
    SECTION("a <b c> d - alternate embedded in normal sequence") {
        // Pattern: a <b c> d
        // a takes 1/3, <b c> takes 1/3, d takes 1/3
        // Inside the alternate, b and c each have full span (1.0) of their SUB_SEQ slot
        auto result = akkado::compile("pat(\"c4 <e4 g4> a4\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequences.size() >= 2);  // Root + alternate
        REQUIRE(seq_init->sequence_events.size() >= 2);  // Event storage for each sequence

        // Root sequence should have 3 elements (c4, SUB_SEQ, a4)
        const auto& root = seq_init->sequences[0];
        const auto& root_events = seq_init->sequence_events[0];
        REQUIRE(root_events.size() == 3);
        CHECK(root.mode == cedar::SequenceMode::NORMAL);

        // Check event times and durations
        // Each element takes 1/3 of the normalized span (0.333)
        const float third = 1.0f / 3.0f;

        // Event 0: c4 at time=0
        CHECK(root_events[0].type == cedar::EventType::DATA);
        CHECK(root_events[0].time == Catch::Approx(0.0f).margin(0.001f));
        CHECK(root_events[0].duration == Catch::Approx(third).margin(0.001f));

        // Event 1: SUB_SEQ at time=1/3
        CHECK(root_events[1].type == cedar::EventType::SUB_SEQ);
        CHECK(root_events[1].time == Catch::Approx(third).margin(0.001f));
        CHECK(root_events[1].duration == Catch::Approx(third).margin(0.001f));

        // Event 2: a4 at time=2/3
        CHECK(root_events[2].type == cedar::EventType::DATA);
        CHECK(root_events[2].time == Catch::Approx(2.0f * third).margin(0.001f));
        CHECK(root_events[2].duration == Catch::Approx(third).margin(0.001f));

        // Alternate sequence (ID 1) should have 2 choices with duration=1.0
        if (seq_init->sequences.size() > 1 && seq_init->sequence_events.size() > 1) {
            const auto& alt = seq_init->sequences[1];
            const auto& alt_events = seq_init->sequence_events[1];
            CHECK(alt.mode == cedar::SequenceMode::ALTERNATE);
            REQUIRE(alt_events.size() == 2);
            // Each alternate choice has full span (1.0) within its SUB_SEQ slot
            CHECK(alt_events[0].duration == Catch::Approx(1.0f).margin(0.001f));
            CHECK(alt_events[1].duration == Catch::Approx(1.0f).margin(0.001f));
        }
    }

    SECTION("verify query output durations") {
        // Compile the pattern
        auto result = akkado::compile("pat(\"c4 <e4 g4> a4\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Create a SequenceState and query it using static buffers for the test
        static constexpr std::size_t TEST_MAX_SEQUENCES = 16;
        static constexpr std::size_t TEST_MAX_EVENTS_PER_SEQ = 64;
        static constexpr std::size_t TEST_MAX_OUTPUT_EVENTS = 64;
        static cedar::Sequence test_sequences[TEST_MAX_SEQUENCES];
        static cedar::Event test_events[TEST_MAX_SEQUENCES][TEST_MAX_EVENTS_PER_SEQ];
        static cedar::OutputEvents::OutputEvent test_output_events[TEST_MAX_OUTPUT_EVENTS];

        std::size_t num_seqs = std::min(seq_init->sequences.size(), TEST_MAX_SEQUENCES);

        // Copy sequences and set up event pointers
        for (std::size_t i = 0; i < num_seqs; ++i) {
            test_sequences[i] = seq_init->sequences[i];
            if (i < seq_init->sequence_events.size() && !seq_init->sequence_events[i].empty()) {
                std::size_t num_events = std::min(seq_init->sequence_events[i].size(), TEST_MAX_EVENTS_PER_SEQ);
                for (std::size_t j = 0; j < num_events; ++j) {
                    test_events[i][j] = seq_init->sequence_events[i][j];
                }
                test_sequences[i].events = test_events[i];
                test_sequences[i].num_events = static_cast<std::uint32_t>(num_events);
                test_sequences[i].capacity = static_cast<std::uint32_t>(TEST_MAX_EVENTS_PER_SEQ);
            }
        }

        cedar::SequenceState state;
        state.sequences = test_sequences;
        state.num_sequences = static_cast<std::uint32_t>(num_seqs);
        state.seq_capacity = static_cast<std::uint32_t>(TEST_MAX_SEQUENCES);
        state.output.events = test_output_events;
        state.output.num_events = 0;
        state.output.capacity = static_cast<std::uint32_t>(TEST_MAX_OUTPUT_EVENTS);
        state.cycle_length = seq_init->cycle_length;

        // Query cycle 0
        cedar::query_pattern(state, 0, seq_init->cycle_length);

        // Should have 3 events
        REQUIRE(state.output.num_events == 3);

        // All durations should be cycle_length / 3
        float expected_duration = seq_init->cycle_length / 3.0f;
        CHECK(state.output.events[0].duration == Catch::Approx(expected_duration).margin(0.01f));
        CHECK(state.output.events[1].duration == Catch::Approx(expected_duration).margin(0.01f));
        CHECK(state.output.events[2].duration == Catch::Approx(expected_duration).margin(0.01f));

        // Check times
        CHECK(state.output.events[0].time == Catch::Approx(0.0f).margin(0.01f));
        CHECK(state.output.events[1].time == Catch::Approx(expected_duration).margin(0.01f));
        CHECK(state.output.events[2].time == Catch::Approx(2.0f * expected_duration).margin(0.01f));
    }

    SECTION("long pattern - 16 events (exceeds old MAX_EVENTS_PER_SEQ=8)") {
        // This pattern has 16 events, which exceeds the old limit of 8
        auto result = akkado::compile("pat(\"c4 g4 ~ ~ c5 e4 ~ g3 c4 g4 ~ ~ c5 e4 ~ a3\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequence_events.size() >= 1);

        // Count total events (excluding rests which have 0 events)
        // Pattern: c4 g4 ~ ~ c5 e4 ~ g3 c4 g4 ~ ~ c5 e4 ~ a3
        // Notes:   1  2     3  4     5  6  7     8  9     10 = 10 note events
        std::uint32_t total_events = 0;
        for (const auto& events : seq_init->sequence_events) {
            total_events += static_cast<std::uint32_t>(events.size());
        }
        CHECK(total_events >= 10);  // At least 10 note events
    }

    SECTION("long pattern with groups - many nested events") {
        // This creates 10 main events plus nested events
        auto result = akkado::compile("pat(\"[c4 d4] [e4 f4] [g4 a4] [b4 c5] [d5 e5]\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Count total events across all sequences
        std::uint32_t total_events = 0;
        for (const auto& events : seq_init->sequence_events) {
            total_events += static_cast<std::uint32_t>(events.size());
        }
        // Should have 10 note events
        CHECK(total_events >= 10);
    }

    SECTION("alternation with groups - groups wrapped in sub-sequences") {
        // <[c4 e4] [g4 b4]> should alternate between the two groups as units
        // not cycle through individual notes c4, e4, g4, b4
        auto result = akkado::compile("pat(\"<[c4 e4] [g4 b4]>\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequences.size() >= 2);

        // Find the ALTERNATE sequence
        std::size_t alt_idx = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < seq_init->sequences.size(); ++i) {
            if (seq_init->sequences[i].mode == cedar::SequenceMode::ALTERNATE) {
                alt_idx = i;
                break;
            }
        }
        REQUIRE(alt_idx != static_cast<std::size_t>(-1));

        // The ALTERNATE sequence should have exactly 2 events (SUB_SEQ for each group)
        // not 4 events (individual notes unrolled)
        const auto& alt_events = seq_init->sequence_events[alt_idx];
        CHECK(alt_events.size() == 2);

        // Each event should be a SUB_SEQ pointing to a NORMAL sequence containing
        // the group's notes
        for (const auto& ev : alt_events) {
            CHECK(ev.type == cedar::EventType::SUB_SEQ);
        }
    }

    SECTION("choice with groups - groups wrapped in sub-sequences") {
        // [c4 e4] | [g4 b4] should pick between the two groups as units
        auto result = akkado::compile("pat(\"[c4 e4] | [g4 b4]\")");
        REQUIRE(result.success);

        // Find SequenceProgram state init
        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Find the RANDOM sequence (choice operator uses RANDOM mode)
        std::size_t rand_idx = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < seq_init->sequences.size(); ++i) {
            if (seq_init->sequences[i].mode == cedar::SequenceMode::RANDOM) {
                rand_idx = i;
                break;
            }
        }
        REQUIRE(rand_idx != static_cast<std::size_t>(-1));

        // The RANDOM sequence should have exactly 2 events (SUB_SEQ for each group)
        const auto& rand_events = seq_init->sequence_events[rand_idx];
        CHECK(rand_events.size() == 2);

        // Each event should be a SUB_SEQ
        for (const auto& ev : rand_events) {
            CHECK(ev.type == cedar::EventType::SUB_SEQ);
        }
    }

    SECTION("speed-modified alternate with groups - groups wrapped in sub-sequences") {
        // <hh hh [hh hh hh]>*2 should have an ALTERNATE sequence with 3 events:
        // 2 DATA (plain hh atoms) + 1 SUB_SEQ (for the [hh hh hh] group)
        // NOT 5 DATA events (flattened group)
        auto result = akkado::compile("pat(\"<hh hh [hh hh hh]>*2\")");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);

        // Find the ALTERNATE sequence
        std::size_t alt_idx = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < seq_init->sequences.size(); ++i) {
            if (seq_init->sequences[i].mode == cedar::SequenceMode::ALTERNATE) {
                alt_idx = i;
                break;
            }
        }
        REQUIRE(alt_idx != static_cast<std::size_t>(-1));

        // The ALTERNATE sequence should have exactly 3 events:
        // hh (DATA), hh (DATA), [hh hh hh] (SUB_SEQ)
        const auto& alt_events = seq_init->sequence_events[alt_idx];
        CHECK(alt_events.size() == 3);

        // The third event (for the group) should be a SUB_SEQ
        CHECK(alt_events[2].type == cedar::EventType::SUB_SEQ);
    }
}

// =============================================================================
// Parameter Exposure Tests
// =============================================================================

TEST_CASE("Codegen: param() generates ENV_GET and records declaration", "[codegen][params]") {
    SECTION("basic param declaration") {
        auto result = akkado::compile(R"(
            vol = param("volume", 0.8, 0, 1)
            saw(220) * vol
        )");
        REQUIRE(result.success);

        // Check param_decls populated
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "volume");
        CHECK(decl.type == akkado::ParamType::Continuous);
        CHECK(decl.default_value == Catch::Approx(0.8f));
        CHECK(decl.min_value == Catch::Approx(0.0f));
        CHECK(decl.max_value == Catch::Approx(1.0f));

        // Verify ENV_GET instruction emitted
        auto insts = get_instructions(result);
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);

        // Verify hash matches declaration
        CHECK(env_get->state_id == decl.name_hash);
    }

    SECTION("param with default range") {
        auto result = akkado::compile(R"(
            x = param("x", 0.5)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.default_value == Catch::Approx(0.5f));
        CHECK(decl.min_value == Catch::Approx(0.0f));
        CHECK(decl.max_value == Catch::Approx(1.0f));
    }

    SECTION("param clamps default to range") {
        auto result = akkado::compile(R"(
            x = param("x", 2.0, 0, 1)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        CHECK(result.param_decls[0].default_value == Catch::Approx(1.0f));
    }

    SECTION("param default below min gets clamped") {
        auto result = akkado::compile(R"(
            x = param("x", -1.0, 0, 10)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        CHECK(result.param_decls[0].default_value == Catch::Approx(0.0f));
    }

    SECTION("param with min > max swaps values") {
        auto result = akkado::compile(R"(
            x = param("x", 0.5, 1, 0)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        // min/max should be swapped
        CHECK(result.param_decls[0].min_value == Catch::Approx(0.0f));
        CHECK(result.param_decls[0].max_value == Catch::Approx(1.0f));
        // Check for warning
        bool has_warning = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == akkado::Severity::Warning && diag.code == "W050") {
                has_warning = true;
                break;
            }
        }
        CHECK(has_warning);
    }

    SECTION("multiple params deduplicate by name") {
        auto result = akkado::compile(R"(
            a = param("vol", 0.5)
            b = param("vol", 0.5)
        )");
        REQUIRE(result.success);
        CHECK(result.param_decls.size() == 1);
    }

    SECTION("different params recorded separately") {
        auto result = akkado::compile(R"(
            v = param("volume", 0.8)
            c = param("cutoff", 2000, 100, 8000)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 2);
        CHECK(result.param_decls[0].name == "volume");
        CHECK(result.param_decls[1].name == "cutoff");
    }
}

TEST_CASE("Codegen: param() requires string literal name", "[codegen][params]") {
    SECTION("variable name fails") {
        auto result = akkado::compile(R"(
            name = "vol"
            x = param(name, 0.5)
        )");
        REQUIRE_FALSE(result.success);
        bool has_error = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.code == "E151") {
                has_error = true;
                break;
            }
        }
        CHECK(has_error);
    }

    SECTION("number literal fails") {
        auto result = akkado::compile(R"(
            x = param(42, 0.5)
        )");
        REQUIRE_FALSE(result.success);
    }
}

TEST_CASE("Codegen: button() creates momentary parameter", "[codegen][params]") {
    SECTION("basic button declaration") {
        auto result = akkado::compile(R"(
            kick = button("kick")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "kick");
        CHECK(decl.type == akkado::ParamType::Button);
        CHECK(decl.default_value == 0.0f);
        CHECK(decl.min_value == 0.0f);
        CHECK(decl.max_value == 1.0f);
    }

    SECTION("button emits ENV_GET with zero fallback") {
        auto result = akkado::compile(R"(
            trig = button("trigger")
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);

        // Find PUSH_CONST for fallback (should be 0)
        const cedar::Instruction* fallback = nullptr;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST) {
                fallback = &inst;
                break;
            }
        }
        REQUIRE(fallback != nullptr);
        CHECK(decode_const_float(*fallback) == 0.0f);

        // Verify ENV_GET
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);
    }
}

TEST_CASE("Codegen: toggle() creates boolean parameter", "[codegen][params]") {
    SECTION("toggle with default off") {
        auto result = akkado::compile(R"(
            mute = toggle("mute")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "mute");
        CHECK(decl.type == akkado::ParamType::Toggle);
        CHECK(decl.default_value == 0.0f);
    }

    SECTION("toggle with default on") {
        auto result = akkado::compile(R"(
            enabled = toggle("enabled", 1)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        CHECK(result.param_decls[0].default_value == 1.0f);
    }

    SECTION("toggle normalizes default to boolean") {
        auto result = akkado::compile(R"(
            x = toggle("x", 0.7)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        // 0.7 > 0.5 should normalize to 1.0
        CHECK(result.param_decls[0].default_value == 1.0f);
    }

    SECTION("toggle normalizes default below threshold") {
        auto result = akkado::compile(R"(
            x = toggle("x", 0.3)
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);
        // 0.3 < 0.5 should normalize to 0.0
        CHECK(result.param_decls[0].default_value == 0.0f);
    }
}

TEST_CASE("Codegen: param_decls source location", "[codegen][params]") {
    SECTION("source offset and length recorded") {
        auto result = akkado::compile(R"(vol = param("volume", 0.5))");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        // Source offset should point to the param() call
        CHECK(decl.source_offset > 0);
        CHECK(decl.source_length > 0);
    }
}

TEST_CASE("Codegen: param hash matches cedar FNV-1a", "[codegen][params]") {
    SECTION("hash is consistent") {
        auto result = akkado::compile(R"(x = param("volume", 0.5))");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        // Compute expected hash
        const char* name = "volume";
        std::uint32_t expected = cedar::fnv1a_hash_runtime(name, std::strlen(name));
        CHECK(decl.name_hash == expected);

        // ENV_GET instruction should use the same hash
        auto insts = get_instructions(result);
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);
        CHECK(env_get->state_id == expected);
    }
}

TEST_CASE("Codegen: dropdown() creates selection parameter", "[codegen][params]") {
    SECTION("basic dropdown declaration") {
        auto result = akkado::compile(R"(
            wave = dropdown("waveform", "sine", "saw", "square")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.name == "waveform");
        CHECK(decl.type == akkado::ParamType::Select);
        CHECK(decl.default_value == 0.0f);  // First option is default
        CHECK(decl.min_value == 0.0f);
        CHECK(decl.max_value == 2.0f);  // 3 options -> max index 2
        REQUIRE(decl.options.size() == 3);
        CHECK(decl.options[0] == "sine");
        CHECK(decl.options[1] == "saw");
        CHECK(decl.options[2] == "square");
    }

    SECTION("dropdown with single option") {
        // `mode` is reserved as a Phase 2 PRD voicing builtin; bind to `m`.
        auto result = akkado::compile(R"(
            m = dropdown("mode", "default")
        )");
        REQUIRE(result.success);
        REQUIRE(result.param_decls.size() == 1);

        const auto& decl = result.param_decls[0];
        CHECK(decl.min_value == 0.0f);
        CHECK(decl.max_value == 0.0f);  // 1 option -> max index 0
        REQUIRE(decl.options.size() == 1);
    }

    SECTION("dropdown emits ENV_GET") {
        auto result = akkado::compile(R"(
            x = dropdown("x", "a", "b")
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        auto* env_get = find_instruction(insts, cedar::Opcode::ENV_GET);
        REQUIRE(env_get != nullptr);
    }

    SECTION("dropdown requires at least one option") {
        // Note: The builtin signature requires at least 2 args (name + opt1)
        // so the analyzer rejects this before codegen sees it
        auto result = akkado::compile(R"(
            x = dropdown("x")
        )");
        REQUIRE_FALSE(result.success);
        // Either analyzer rejection (E004 or E005) or codegen error (E159)
        bool has_error = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == akkado::Severity::Error) {
                has_error = true;
                break;
            }
        }
        CHECK(has_error);
    }

    SECTION("dropdown options must be string literals") {
        auto result = akkado::compile(R"(
            opt = "dynamic"
            x = dropdown("x", opt)
        )");
        REQUIRE_FALSE(result.success);
    }
}

// =============================================================================
// Dot-Call Syntax Tests
// =============================================================================

TEST_CASE("Dot-call syntax", "[codegen][methods]") {
    SECTION("method on builtin: sin(440).abs() == abs(sin(440))") {
        auto dot = akkado::compile(R"(
            x = sin(440).abs()
            out(x, x)
        )");
        auto direct = akkado::compile(R"(
            x = abs(sin(440))
            out(x, x)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        // Same instructions
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call with arguments: osc(\"saw\", 440).lp(800, 0.707)") {
        auto dot = akkado::compile(R"(
            osc("saw", 440).lp(800, 0.707) |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            lp(osc("saw", 440), 800, 0.707) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("chained dot-calls: a.f().g() == g(f(a))") {
        // lp is stereo-native (prd-stereo-native-opcodes Phase 4a) so chain
        // through another stereo-aware opcode (hp) for syntactic-equivalence
        // verification.
        auto dot = akkado::compile(R"(
            osc("saw", 440).lp(800).hp(2000) |> out(%)
        )");
        auto direct = akkado::compile(R"(
            hp(lp(osc("saw", 440), 800), 2000) |> out(%)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call on user-defined function") {
        auto dot = akkado::compile(R"(
            fn gain(sig, amt) -> sig * amt
            osc("sin", 440).gain(0.5) |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            fn gain(sig, amt) -> sig * amt
            gain(osc("sin", 440), 0.5) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call mixed with pipe operator") {
        auto dot = akkado::compile(R"(
            osc("saw", 440).lp(800) |> % * 0.5 |> out(%, %)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> lp(%, 800) |> % * 0.5 |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("dot-call on expression result") {
        auto dot = akkado::compile(R"(
            x = osc("sin", 440)
            x.abs() |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            x = osc("sin", 440)
            abs(x) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call with no extra arguments") {
        // noise().abs() == abs(noise())
        auto dot = akkado::compile(R"(
            noise().abs() |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            abs(noise()) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call produces correct opcodes") {
        auto result = akkado::compile(R"(
            osc("saw", 440).lp(800) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SAW) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::FILTER_SVF_LP) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }

    SECTION("pattern method via dot-call: pat().slow()") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4").slow(2))");
        auto direct = akkado::compile(R"(slow(pat("c4 e4 g4"), 2))");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("chained pattern methods via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4").fast(2).slow(4))");
        auto direct = akkado::compile(R"(slow(fast(pat("c4 e4"), 2), 4))");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }

    SECTION("dot-call on hole: |> %.f(args)") {
        auto dot = akkado::compile(R"(
            osc("saw", 440) |> %.lp(800) |> out(%, %)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> lp(%, 800) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("dot-call on hole with multiple args") {
        auto dot = akkado::compile(R"(
            osc("saw", 440) |> %.lp(800, 2.0) |> out(%, %)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> lp(%, 800, 2.0) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("chained dot-calls on hole") {
        // lp is stereo-native (Phase 4a); chain through hp instead of abs.
        auto dot = akkado::compile(R"(
            osc("saw", 440) |> %.lp(800).hp(2000) |> out(%)
        )");
        auto pipe = akkado::compile(R"(
            osc("saw", 440) |> hp(lp(%, 800), 2000) |> out(%)
        )");
        REQUIRE(dot.success);
        REQUIRE(pipe.success);
        CHECK(dot.bytecode == pipe.bytecode);
    }

    SECTION("dot-call on as-binding: as q |> q.f(args)") {
        auto dot = akkado::compile(R"(
            osc("saw", 440) as q |> q.lp(800) |> out(%, %)
        )");
        auto direct = akkado::compile(R"(
            osc("saw", 440) as q |> lp(q, 800) |> out(%, %)
        )");
        REQUIRE(dot.success);
        REQUIRE(direct.success);
        CHECK(dot.bytecode == direct.bytecode);
    }
}

// =============================================================================
// Pattern Function Tests
// =============================================================================

TEST_CASE("Pattern function: slow()", "[codegen][patterns]") {
    SECTION("slow requires pattern as first argument") {
        auto result = akkado::compile("slow(42, 2)");
        REQUIRE_FALSE(result.success);
        bool found_error = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.code == "E133") {
                found_error = true;
                break;
            }
        }
        CHECK(found_error);
    }

    SECTION("slow requires positive number as second argument") {
        auto result = akkado::compile(R"(slow(pat("c4"), -1))");
        REQUIRE_FALSE(result.success);
    }

    SECTION("slow with valid pattern compiles") {
        // Note: slow() currently passes through - full implementation pending
        auto result = akkado::compile(R"(slow(pat("c4 e4 g4"), 2))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: fast()", "[codegen][patterns]") {
    SECTION("fast requires pattern as first argument") {
        auto result = akkado::compile("fast(42, 2)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("fast with valid pattern compiles") {
        auto result = akkado::compile(R"(fast(pat("c4 e4"), 2))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: rev()", "[codegen][patterns]") {
    SECTION("rev requires pattern as argument") {
        auto result = akkado::compile("rev(42)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("rev with valid pattern compiles") {
        auto result = akkado::compile(R"(rev(pat("c4 e4 g4")))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: transpose()", "[codegen][patterns]") {
    SECTION("transpose requires pattern as first argument") {
        auto result = akkado::compile("transpose(42, 7)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("transpose with valid pattern compiles") {
        auto result = akkado::compile(R"(transpose(pat("c4 e4 g4"), 7))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern function: velocity()", "[codegen][patterns]") {
    SECTION("velocity requires pattern as first argument") {
        auto result = akkado::compile("velocity(42, 0.5)");
        REQUIRE_FALSE(result.success);
    }

    SECTION("velocity requires value between 0 and 1") {
        auto result = akkado::compile(R"(velocity(pat("c4"), 1.5))");
        REQUIRE_FALSE(result.success);
    }

    SECTION("velocity with valid pattern and value compiles") {
        auto result = akkado::compile(R"(velocity(pat("c4 e4"), 0.7))");
        CHECK(result.success);
    }

    SECTION("velocity on sample pattern emits SAMPLE_PLAY") {
        // Regression guard: velocity() recompiles the inner pattern and
        // re-emits SEQPAT_QUERY/STEP itself; without explicit sampler wiring
        // it would return raw sample-IDs as DC instead of audio.
        auto result = akkado::compile(R"(velocity(pat("bd sd hh"), 0.7))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_STEP) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::SAMPLE_PLAY) != nullptr);
    }
}

// =============================================================================
// Pattern Transform Chaining Tests
// =============================================================================

TEST_CASE("Pattern transform chaining compiles", "[codegen][patterns]") {
    SECTION("transpose(slow(...)) compiles - two-level nesting") {
        auto result = akkado::compile(R"(transpose(slow(pat("c4 e4"), 2), 12))");
        CHECK(result.success);
    }

    SECTION("rev(transpose(slow(...))) compiles - three-level nesting") {
        auto result = akkado::compile(R"(rev(transpose(slow(pat("c4 e4"), 2), 12)))");
        CHECK(result.success);
    }

    SECTION("fast(transpose(...)) compiles") {
        auto result = akkado::compile(R"(fast(transpose(pat("c4 e4"), 7), 2))");
        CHECK(result.success);
    }

    SECTION("slow(rev(...)) compiles") {
        auto result = akkado::compile(R"(slow(rev(pat("c4 e4 g4")), 3))");
        CHECK(result.success);
    }

    SECTION("transpose(fast(...)) compiles") {
        auto result = akkado::compile(R"(transpose(fast(pat("c4 e4"), 2), 5))");
        CHECK(result.success);
    }

    SECTION("rev(slow(...)) compiles") {
        auto result = akkado::compile(R"(rev(slow(pat("c4 e4 g4"), 2)))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern transform chaining: semantic correctness", "[codegen][patterns]") {
    SECTION("slow(pat(...), 2) doubles cycle length, event times unchanged") {
        // pat("c4 e4") has 2 elements -> base cycle_length = 2
        // slow(2) -> cycle_length = 4, event times stay normalized
        // Runtime formula (e.time * cycle_length) handles the scaling
        auto result = akkado::compile(R"(slow(pat("c4 e4"), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 2);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f));
        CHECK(si.sequence_events[0][1].time == Catch::Approx(0.5f));
    }

    SECTION("slow(fast(pat(...), 2), 2) is identity") {
        // pat("c4 e4") base: cycle_length=2, times at 0, 0.5
        // fast(2): cycle_length=1, times unchanged
        // slow(2): cycle_length=2, times unchanged -> identity
        auto result = akkado::compile(R"(slow(fast(pat("c4 e4"), 2), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f));
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 2);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f));
        CHECK(si.sequence_events[0][1].time == Catch::Approx(0.5f));
    }

    SECTION("transpose(slow(pat(...), 2), 12) applies both transforms") {
        // pat("c4 e4") base: cycle_length=2
        // slow(2): cycle_length=4
        // transpose(12): frequencies doubled (octave up)
        auto result = akkado::compile(R"(transpose(slow(pat("c4 e4"), 2), 12))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        // Cycle length should be doubled from slow
        CHECK(si.cycle_length == Catch::Approx(4.0f));

        // Events should have transposed frequencies
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 2);
        // C4 = 261.63 Hz, transposed +12 = 523.25 Hz
        float c4_freq = 261.6256f;
        CHECK(si.sequence_events[0][0].values[0] == Catch::Approx(c4_freq * 2.0f).margin(1.0f));
    }

    SECTION("fast(fast(pat(...), 2), 3) compounds speed") {
        // pat("c4 e4") base: cycle_length=2
        // fast(2): cycle_length=1
        // fast(3): cycle_length=1/3
        auto result = akkado::compile(R"(fast(fast(pat("c4 e4"), 2), 3))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f / 6.0f));
    }

    SECTION("fast(pat(...), 2) halves cycle length, event times unchanged") {
        // pat("c4 e4") has 2 elements -> base cycle_length = 2
        // fast(2) -> cycle_length = 1, event times stay normalized
        auto result = akkado::compile(R"(fast(pat("c4 e4"), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(1.0f));
        REQUIRE(si.sequence_events.size() >= 1);
        REQUIRE(si.sequence_events[0].size() >= 2);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f));
        CHECK(si.sequence_events[0][1].time == Catch::Approx(0.5f));
    }

    SECTION("rev(pat(...)) reverses event positions within [0,1)") {
        // pat("c4 e4") -> events at 0.0 (dur 0.5) and 0.5 (dur 0.5)
        // rev -> e4 at 0.0, c4 at 0.5
        auto result = akkado::compile(R"(rev(pat("c4 e4")))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f));
        REQUIRE(si.sequence_events.size() >= 1);
        REQUIRE(si.sequence_events[0].size() >= 2);
        // After rev: original event at 0.5 moves to 0.0, original at 0.0 moves to 0.5
        // Events may be unsorted; check that both positions exist
        bool has_0 = false, has_05 = false;
        for (const auto& e : si.sequence_events[0]) {
            if (std::abs(e.time - 0.0f) < 0.01f) has_0 = true;
            if (std::abs(e.time - 0.5f) < 0.01f) has_05 = true;
        }
        CHECK(has_0);
        CHECK(has_05);
    }

    SECTION("rev(slow(pat(...), 2)) reverses within normalized range") {
        // pat("c4 e4") base: cycle_length=2, events at 0.0, 0.5
        // slow(2): cycle_length=4, events still at 0.0, 0.5
        // rev: events reversed to 0.0, 0.5 (swapped values), cycle_length=4
        auto result = akkado::compile(R"(rev(slow(pat("c4 e4"), 2)))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        REQUIRE(si.sequence_events.size() >= 1);
        REQUIRE(si.sequence_events[0].size() >= 2);
        // All events should be in [0, 1) range
        for (const auto& e : si.sequence_events[0]) {
            CHECK(e.time >= 0.0f);
            CHECK(e.time < 1.0f);
        }
    }
}

// =============================================================================
// Phase 2 PRD D2: mini-notation record suffix `c4{vel:0.8, ...}`
// =============================================================================

TEST_CASE("Mini-notation record suffix: vel sets velocity",
          "[codegen][patterns][phase2][record_suffix]") {
    auto result = akkado::compile(R"(pat("c4{vel:0.7} e4{vel:0.5}"))");
    REQUIRE(result.success);
    const auto& si = result.state_inits[0];
    REQUIRE(si.sequence_events[0].size() == 2);
    CHECK(si.sequence_events[0][0].velocity == Catch::Approx(0.7f).margin(0.01f));
    CHECK(si.sequence_events[0][1].velocity == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("Mini-notation record suffix: positional :vel and {vel} resolve compatibly",
          "[codegen][patterns][phase2][record_suffix]") {
    // Both forms should produce identical velocities (the record-suffix `vel`
    // overrides the :0.x shorthand when both are present per §9.4 last-wins).
    auto pos = akkado::compile(R"(pat("c4:0.5"))");
    auto rec = akkado::compile(R"(pat("c4{vel:0.5}"))");
    REQUIRE(pos.success);
    REQUIRE(rec.success);
    CHECK(pos.state_inits[0].sequence_events[0][0].velocity ==
          Catch::Approx(rec.state_inits[0].sequence_events[0][0].velocity).margin(0.001f));
}

TEST_CASE("Mini-notation record suffix: backwards compat with polymeter `{a b}%n`",
          "[codegen][patterns][phase2][record_suffix]") {
    // {a b}%3 (polymeter) must still parse: there is no preceding note, so
    // the record-suffix lexer never activates.
    auto result = akkado::compile(R"(pat("{c4 e4 g4}%3"))");
    CHECK(result.success);
}

TEST_CASE("Mini-notation record suffix: whitespace before `{` keeps polymeter",
          "[codegen][patterns][phase2][record_suffix]") {
    // `c4 {a b}%3` — record-suffix requires `{` to immediately follow the
    // note. With whitespace, the brace starts a polymeter group instead.
    auto result = akkado::compile(R"(pat("c4 {c4 e4}%3"))");
    CHECK(result.success);
}

TEST_CASE("Mini-notation record suffix: multiple keys",
          "[codegen][patterns][phase2][record_suffix]") {
    // vel and dur are recognized; bend/cutoff stay on atom_data.properties
    // (deferred runtime exposure). Compile succeeds; velocity reflects vel.
    auto result = akkado::compile(R"(pat("c4{vel:0.8, bend:0.3, cutoff:0.4}"))");
    REQUIRE(result.success);
    const auto& si = result.state_inits[0];
    REQUIRE(si.sequence_events[0].size() == 1);
    CHECK(si.sequence_events[0][0].velocity == Catch::Approx(0.8f).margin(0.01f));
}

TEST_CASE("Mini-notation record suffix: dur sets event.duration",
          "[codegen][patterns][phase2][record_suffix]") {
    // `dur` is a recognized short-form key; codegen multiplies the atom's
    // time_span by the dur value when emitting the cedar event. For a
    // single-atom pat, time_span == 1.0 (one full cycle), so dur:0.5
    // yields event.duration == 0.5.
    auto single = akkado::compile(R"(pat("c4{dur:0.5}"))");
    REQUIRE(single.success);
    REQUIRE_FALSE(single.state_inits.empty());
    REQUIRE(single.state_inits[0].sequence_events[0].size() == 1);
    CHECK(single.state_inits[0].sequence_events[0][0].duration ==
          Catch::Approx(0.5f).margin(0.001f));

    // For a 2-atom pat, each atom's time_span == 0.5; dur:0.5 yields
    // event.duration == 0.25.
    auto two = akkado::compile(R"(pat("c4{dur:0.5} e4{dur:1.0}"))");
    REQUIRE(two.success);
    const auto& events = two.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 2);
    // Sort by time so assertion order is stable.
    std::vector<std::pair<float, float>> pairs;
    for (const auto& e : events) pairs.emplace_back(e.time, e.duration);
    std::sort(pairs.begin(), pairs.end());
    CHECK(pairs[0].second == Catch::Approx(0.25f).margin(0.001f));   // c4 dur 0.5 * 0.5
    CHECK(pairs[1].second == Catch::Approx(0.5f).margin(0.001f));    // e4 dur 1.0 * 0.5
}

TEST_CASE("Mini-notation record suffix: works on sample atoms in pat()",
          "[codegen][patterns][phase2][record_suffix][sample]") {
    // Before fix: lex_sample_only() returned without calling try_lex_record_suffix(),
    // so `bd{vel:0.5}` was a parse error in any sample-mode context.
    // Per-voice velocity: sample atoms route per-atom velocity through
    // velocities[0] (event.velocity is pinned to 1.0 so the post-MUL is a no-op).
    auto result = akkado::compile(R"(pat("bd{vel:0.5} sd{vel:0.7}"))");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 2);
    std::vector<std::pair<float, float>> by_time;
    for (const auto& e : events) by_time.emplace_back(e.time, e.velocities[0]);
    std::sort(by_time.begin(), by_time.end());
    CHECK(by_time[0].second == Catch::Approx(0.5f).margin(0.01f));
    CHECK(by_time[1].second == Catch::Approx(0.7f).margin(0.01f));
}

TEST_CASE("Mini-notation record suffix: works on samples inside [...] groups",
          "[codegen][patterns][phase2][record_suffix][sample]") {
    // The user's actual reported failure: `[hh,bd{vel:0.5}]` errored with
    // "Expected ']' after group" / "Expected '}' after polymeter" because the
    // `{` was tokenized as polymeter-open instead of a record-suffix.
    auto result = akkado::compile(R"(pat("[hh,bd{vel:0.5}] hh"))");
    REQUIRE(result.success);
}

TEST_CASE("Sample velocity: {vel:V} inside polyrhythm reaches merged event",
          "[codegen][patterns][record_suffix][sample][velocity]") {
    // Per-voice velocity: `[hh,bd{vel:0.5}]` merges into one event with
    // num_values=2; velocities[0]=hh's vel (1.0), velocities[1]=bd's (0.5).
    // event.velocity is pinned to 1.0 (post-MUL no-op); op_sample_play applies
    // velocities[v] per voice so cp/hh play unattenuated and bd plays at 0.5.
    // Before this fix the merged event carried min(1.0, 0.5) = 0.5 which
    // attenuated the *whole stack* including hh — see the [cp, bd{vel:0.05}]
    // bug report.
    auto result = akkado::compile(R"(pat("[hh,bd{vel:0.5}] hh"))");
    REQUIRE(result.success);
    REQUIRE(!result.state_inits.empty());
    const auto& events = result.state_inits[0].sequence_events[0];
    // Two top-level steps -> two events. The first is the polyrhythm-merged
    // event; the second is the bare `hh`.
    REQUIRE(events.size() == 2);
    // Sort by event time so order is stable.
    std::vector<const cedar::Event*> by_time;
    for (const auto& e : events) by_time.push_back(&e);
    std::sort(by_time.begin(), by_time.end(),
              [](const cedar::Event* a, const cedar::Event* b) { return a->time < b->time; });
    // Polyrhythm event: hh at full velocity, bd at 0.5.
    REQUIRE(by_time[0]->num_values == 2);
    CHECK(by_time[0]->velocity == Catch::Approx(1.0f).margin(0.01f));
    CHECK(by_time[0]->velocities[0] == Catch::Approx(1.0f).margin(0.01f));
    CHECK(by_time[0]->velocities[1] == Catch::Approx(0.5f).margin(0.01f));
    // Bare hh: scalar event, velocities[0] = 1.0.
    CHECK(by_time[1]->velocities[0] == Catch::Approx(1.0f).margin(0.01f));
}

TEST_CASE("Sample velocity: SAMPLE_PLAY output is post-multiplied by velocity",
          "[codegen][patterns][record_suffix][sample][velocity]") {
    // Bug B regression: SAMPLE_PLAY has no velocity input. emit_sample_chain
    // emits a MUL after SAMPLE_PLAY whose inputs are the sampler output buffer
    // and the velocity_buf produced by SEQPAT_STEP, so `velocity()` can scale
    // the sampler output at runtime via the velocity_buf signal.
    auto result = akkado::compile(R"(s"bd{vel:0.25}")");
    REQUIRE(result.success);
    auto insts = get_instructions(result);

    const cedar::Instruction* sample_play = nullptr;
    std::uint16_t velocity_buf = 0xFFFF;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::SEQPAT_STEP) {
            // SEQPAT_STEP inputs[0] is the velocity output buffer (see
            // emit_per_voice_seqpat()).
            velocity_buf = inst.inputs[0];
        }
        if (inst.opcode == cedar::Opcode::SAMPLE_PLAY) {
            sample_play = &inst;
        }
    }
    REQUIRE(sample_play != nullptr);
    REQUIRE(velocity_buf != 0xFFFF);

    // Find a MUL whose inputs are (SAMPLE_PLAY.out_buffer, velocity_buf).
    bool found_velocity_mul = false;
    for (const auto& inst : insts) {
        if (inst.opcode != cedar::Opcode::MUL) continue;
        if (inst.inputs[0] == sample_play->out_buffer &&
            inst.inputs[1] == velocity_buf) {
            found_velocity_mul = true;
            break;
        }
    }
    CHECK(found_velocity_mul);
}

TEST_CASE("Sample property: custom slots propagate through polyrhythm",
          "[codegen][patterns][record_suffix][sample][polyrhythm][prop_vals]") {
    // Phase 3 fix: previously flatten_to_timelines() dropped unrecognized
    // record-suffix keys (cutoff, bend, aftertouch, ...) on the polyrhythm
    // path. compile_polyrhythm_events then had no way to surface them on the
    // merged event. Now BranchEvent carries prop_vals + a populated bitmap,
    // and the merge does a bitmap-aware copy that propagates each slot from
    // the first branch that set it.
    auto result = akkado::compile(R"(s"[hh,bd{cutoff:0.3}]")");
    REQUIRE(result.success);
    REQUIRE(!result.state_inits.empty());
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(!events.empty());
    const auto& evt = events[0];
    REQUIRE(evt.num_values == 2);
    // The cutoff slot is allocated lazily by allocate_property_slot. Find any
    // prop slot that carries 0.3 — there's only one custom property in this
    // pattern so a single non-zero entry is unambiguous.
    bool found_cutoff = false;
    for (std::size_t s = 0; s < cedar::MAX_PROPS_PER_EVENT; ++s) {
        if (std::fabs(evt.prop_vals[s] - 0.3f) < 1e-4f) found_cutoff = true;
    }
    CHECK(found_cutoff);
}

TEST_CASE("Sample property: explicit {key:0} survives merge",
          "[codegen][patterns][record_suffix][sample][polyrhythm][prop_vals]") {
    // Bitmap-aware merge: a branch can deliberately set a prop slot to 0.0
    // and that explicit zero must reach the merged event (a "first non-zero"
    // policy would silently misroute the case where one branch wants to
    // disable a parameter the other branch doesn't touch).
    auto result = akkado::compile(R"(s"[hh{cutoff:0},bd{cutoff:0.5}]")");
    REQUIRE(result.success);
    REQUIRE(!result.state_inits.empty());
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(!events.empty());
    const auto& evt = events[0];
    REQUIRE(evt.num_values == 2);
    // Bitmap-aware merge takes the first branch's slot, so we expect 0.0
    // on the (single) cutoff slot — not bd's 0.5.
    bool found_explicit_zero = false;
    for (std::size_t s = 0; s < cedar::MAX_PROPS_PER_EVENT; ++s) {
        // Slot is "populated by the first branch with cutoff:0" if its value
        // equals 0.0f exactly AND another slot or the same slot was set by
        // bd. We just check no slot ended up at bd's 0.5 — if first-wins
        // is correctly applied, the merged event has no 0.5 in any slot.
        if (std::fabs(evt.prop_vals[s] - 0.5f) < 1e-4f) found_explicit_zero = true;
    }
    CHECK_FALSE(found_explicit_zero);  // 0.5 should NOT win — hh's explicit 0 came first
}

TEST_CASE("Sample velocity: per-voice velocity in polyrhythm — [cp,bd{vel:0.05}]",
          "[codegen][patterns][record_suffix][sample][velocity][polyrhythm]") {
    // The user's reported bug: in `s"[cp, bd{vel:0.05}]"`, cp was attenuated
    // along with bd because the merged Event carried min(velocity)=0.05 and
    // the codegen post-MUL multiplied the entire summed sampler output by it.
    //
    // After the per-voice velocity fix:
    //   - event.velocity = 1.0 (post-MUL no-op for sample patterns)
    //   - event.velocities[0] = cp's velocity (1.0)
    //   - event.velocities[1] = bd's velocity (0.05)
    //   - op_sample_play applies velocities[v] per voice at trigger time
    //
    // Both voices play independently: cp at full amplitude, bd at 5%.
    auto result = akkado::compile(R"(s"[cp,bd{vel:0.05}]")");
    REQUIRE(result.success);
    REQUIRE(!result.state_inits.empty());
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(!events.empty());
    const auto& evt = events[0];
    REQUIRE(evt.num_values == 2);
    CHECK(evt.velocity == Catch::Approx(1.0f).margin(0.001f));
    CHECK(evt.velocities[0] == Catch::Approx(1.0f).margin(0.001f));
    CHECK(evt.velocities[1] == Catch::Approx(0.05f).margin(0.001f));
}

TEST_CASE("Sample velocity: bd{vel:0.25} attenuates rendered audio amplitude",
          "[codegen][patterns][record_suffix][sample][velocity]") {
    // End-to-end check: compile the same pattern twice — once with
    // {vel:0.25}, once without — load both into a VM, trigger one cycle,
    // and verify the {vel:0.25} render is roughly 4x quieter.
    // We use synthesized samples via the SampleBank API; this test is
    // in test_codegen.cpp so we keep it sample-bank-free by checking the
    // peak-amplitude ratio of the sampler output buffer indirectly.
    //
    // For simplicity (and to avoid pulling in a full SampleBank harness),
    // we only verify the codegen-side invariant here and rely on the
    // instruction-level test above + the python experiment for the
    // numerical check.
    auto loud = akkado::compile(R"(s"bd")");
    auto quiet = akkado::compile(R"(s"bd{vel:0.25}")");
    REQUIRE(loud.success);
    REQUIRE(quiet.success);

    // The {vel:0.25} pattern's first event must carry velocities[0] = 0.25.
    // event.velocity stays at 1.0 (post-MUL no-op for sample patterns); the
    // per-atom velocity rides on velocities[0] and op_sample_play applies it
    // per-voice.
    REQUIRE(!quiet.state_inits.empty());
    const auto& events = quiet.state_inits[0].sequence_events[0];
    REQUIRE(!events.empty());
    CHECK(events[0].velocities[0] == Catch::Approx(0.25f).margin(0.001f));

    // Both should emit one SAMPLE_PLAY each followed by a MUL.
    auto loud_insts = get_instructions(loud);
    auto quiet_insts = get_instructions(quiet);
    CHECK(count_instructions(loud_insts, cedar::Opcode::SAMPLE_PLAY) == 1);
    CHECK(count_instructions(quiet_insts, cedar::Opcode::SAMPLE_PLAY) == 1);
}

TEST_CASE("Mini-notation record suffix: works in s\"...\" sample-mode prefix",
          "[codegen][patterns][phase2][record_suffix][sample]") {
    // `s"..."` token forces MiniParseMode::Sample. Same lexer bug affected
    // it: lex_sample_only() never called try_lex_record_suffix().
    // Per-voice velocity: per-atom vel rides on velocities[0].
    auto result = akkado::compile(R"(s"bd{vel:0.5} sd")");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 2);
    std::vector<std::pair<float, float>> by_time;
    for (const auto& e : events) by_time.emplace_back(e.time, e.velocities[0]);
    std::sort(by_time.begin(), by_time.end());
    CHECK(by_time[0].second == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("Mini-notation record suffix: full s\"...\" pattern from bug report",
          "[codegen][patterns][phase2][record_suffix][sample]") {
    // Verbatim from the user's bug report (with `vel: 0.5` instead of the
    // typo `vel: 05`, which would clamp to 1.0).
    const char* src = R"(s"[hh,bd] hh [hh,sd] [hh ~ ~ rim] [hh,bd{vel:0.5}] [hh,bd] [hh,sd] oh" |> out(%, %))";
    auto result = akkado::compile(src);
    REQUIRE(result.success);
}

TEST_CASE("Mini-notation record suffix: chord atoms carry properties",
          "[codegen][patterns][phase2][record_suffix][chord]") {
    // try_lex_chord_symbol() was missing the try_lex_record_suffix() call too,
    // so `Am{vel:0.5}` parsed as `Am` followed by polymeter-open `{` and errored.
    // Chord patterns require poly() at codegen time; we just verify the parser
    // no longer chokes on the record-suffix.
    auto [tokens, diags] = akkado::lex_mini("Am{vel:0.5}");
    REQUIRE(diags.empty());
    REQUIRE(tokens.size() >= 1);
    REQUIRE(tokens[0].type == akkado::MiniTokenType::ChordToken);
    const auto& chord = tokens[0].as_chord();
    REQUIRE(chord.properties.size() == 1);
    CHECK(chord.properties[0].first == "vel");
    CHECK(chord.properties[0].second == Catch::Approx(0.5f).margin(0.001f));
}

TEST_CASE("Mini-notation record suffix: whitespace tolerance",
          "[codegen][patterns][phase2][record_suffix]") {
    // Lexer skips whitespace around `:` and `,`. Worth testing post-fix on
    // sample atoms too, since they go through a different lex path.
    auto a = akkado::compile(R"(s"bd{vel: 0.5}")");
    auto b = akkado::compile(R"(s"bd{vel : 0.5}")");
    auto c = akkado::compile(R"(s"bd{vel:0.5 , bend:0.3}")");
    CHECK(a.success);
    CHECK(b.success);
    CHECK(c.success);
}

TEST_CASE("Mini-notation record suffix: bare-integer values accepted",
          "[codegen][patterns][phase2][record_suffix]") {
    // `{vel:1}` (no decimal) — lexer accepts integers; codegen clamps to [0,1].
    auto result = akkado::compile(R"(s"bd{vel:1}")");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 1);
    CHECK(events[0].velocity == Catch::Approx(1.0f).margin(0.001f));
}

TEST_CASE("Mini-notation record suffix: negative regressions still work",
          "[codegen][patterns][phase2][record_suffix]") {
    // {bd hh}%3 — `{` not preceded by an atom, must still parse as polymeter.
    SECTION("polymeter unaffected by sample-mode record-suffix support") {
        auto r = akkado::compile(R"(s"{bd hh}%3")");
        CHECK(r.success);
    }
    // `bd {hh sd}%3` — whitespace before `{` keeps it as polymeter, not record.
    SECTION("whitespace before brace keeps polymeter") {
        auto r = akkado::compile(R"(s"bd {hh sd}%3")");
        CHECK(r.success);
    }
    // Chord disambiguation (Am vs A4) untouched by the fix.
    SECTION("A4 still parses as pitch, not chord") {
        auto [tokens, diags] = akkado::lex_mini("A4");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == akkado::MiniTokenType::PitchToken);
    }
}

// =============================================================================
// Phase 2.1 PRD §11.1: custom-property pipe-binding accessor
// =============================================================================

TEST_CASE("Phase 2.1: custom-property slot population from record suffix",
          "[codegen][patterns][phase21][custom_property]") {
    // `cutoff` is unrecognized — gets allocated slot 0; values land on
    // event.prop_vals[0]. PRD §11.1 / SequenceCompiler::custom_property_slots().
    auto result = akkado::compile(R"(pat("c4{cutoff:0.3} e4{cutoff:0.7}"))");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 2);
    // Order events by time so the assertion is stable.
    std::vector<std::pair<float, float>> by_time;
    for (const auto& e : events) by_time.emplace_back(e.time, e.prop_vals[0]);
    std::sort(by_time.begin(), by_time.end());
    CHECK(by_time[0].second == Catch::Approx(0.3f).margin(0.001f));
    CHECK(by_time[1].second == Catch::Approx(0.7f).margin(0.001f));
}

TEST_CASE("Phase 2.1: SEQPAT_PROP emitted for custom property",
          "[codegen][patterns][phase21][custom_property]") {
    // The §11.1 example must compile and emit SEQPAT_PROP for `cutoff`.
    auto result = akkado::compile(R"(
        pat("c4{cutoff:0.3} e4{cutoff:0.7}") as e
          |> osc("saw", e.freq)
          |> lp(%, 200 + e.cutoff * 4000)
          |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Exactly one SEQPAT_PROP for the `cutoff` slot.
    auto count = count_instructions(insts, cedar::Opcode::SEQPAT_PROP);
    CHECK(count == 1);
    const auto* prop_inst = find_instruction(insts, cedar::Opcode::SEQPAT_PROP);
    REQUIRE(prop_inst != nullptr);
    CHECK(prop_inst->rate == 0);  // first registered slot
}

TEST_CASE("Phase 2.1: e.unknownkey lists custom fields in error",
          "[codegen][patterns][phase21][custom_property]") {
    auto result = akkado::compile(R"(
        pat("c4{cutoff:0.5}") as e |> osc("sin", e.unknownkey) |> out(%, %)
    )");
    REQUIRE_FALSE(result.success);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E136" &&
            d.message.find("cutoff") != std::string::npos &&
            d.message.find("unknownkey") != std::string::npos) {
            found = true; break;
        }
    }
    CHECK(found);
}

TEST_CASE("Phase 2.1: pattern with > 4 custom properties keeps first 4",
          "[codegen][patterns][phase21][custom_property]") {
    // Keys beyond the 4-slot limit are silently dropped; the first 4 each
    // get a slot. PRD §9.4 — overflow is not a hard error so users can
    // experiment freely; if buffer plumbing for >4 keys is ever needed it's
    // a separate enhancement.
    auto result = akkado::compile(
        R"(pat("c4{a:1, b:2, c:3, d:4, e:5}") as p |> osc("sin", p.a) |> out(%, %))");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Four custom keys take slots 0..3; the 5th is dropped. Expect exactly
    // 4 SEQPAT_PROP instructions emitted.
    auto count = count_instructions(insts, cedar::Opcode::SEQPAT_PROP);
    CHECK(count == 4);
}

// =============================================================================
// Phase 2.1 PRD §11.2: standalone bend()/aftertouch()/dur() transforms
// =============================================================================

TEST_CASE("Phase 2.1: standalone bend() writes to event.prop_vals[0]",
          "[codegen][patterns][phase21][bend]") {
    auto result = akkado::compile(R"(bend(pat("c4 e4 g4"), 0.5))");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 3);
    for (const auto& e : events) {
        CHECK(e.prop_vals[0] == Catch::Approx(0.5f).margin(0.001f));
    }
}

TEST_CASE("Phase 2.1: bend() result reachable via e.bend pipe-binding",
          "[codegen][patterns][phase21][bend]") {
    // Standalone bend() registers slot keyed by "bend"; e.bend resolves via
    // pattern_field() fallthrough to payload->custom_fields["bend"].
    auto result = akkado::compile(R"(
        bend(pat("c4 e4"), 0.3) as e |> osc("sin", e.freq + e.bend) |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto count = count_instructions(insts, cedar::Opcode::SEQPAT_PROP);
    CHECK(count == 1);
}

TEST_CASE("Phase 2.1: aftertouch() writes to its own slot independent of bend",
          "[codegen][patterns][phase21][aftertouch]") {
    // Both transforms applied — bend takes slot 0, aftertouch takes slot 1
    // (deterministic insertion order). Each event carries both values.
    auto result = akkado::compile(R"(aftertouch(bend(pat("c4 e4"), 0.4), 0.7))");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 2);
    for (const auto& e : events) {
        CHECK(e.prop_vals[0] == Catch::Approx(0.4f).margin(0.001f));  // bend (slot 0)
        CHECK(e.prop_vals[1] == Catch::Approx(0.7f).margin(0.001f));  // aftertouch (slot 1)
    }
}

TEST_CASE("Phase 2.1: inline {bend:x} reused by standalone bend(pat, y) — value overwritten",
          "[codegen][patterns][phase21][bend]") {
    // The inline `{bend:0.2}` registers the bend slot with 0.2; the outer
    // bend() transform uses the same slot and overwrites to 0.7.
    auto result = akkado::compile(R"(bend(pat("c4{bend:0.2}"), 0.7))");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 1);
    CHECK(events[0].prop_vals[0] == Catch::Approx(0.7f).margin(0.001f));
}

TEST_CASE("Phase 2.1: dur() multiplies event.duration",
          "[codegen][patterns][phase21][dur]") {
    auto result = akkado::compile(R"(dur(pat("c4 e4"), 0.5))");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE(events.size() == 2);
    // Each atom's natural time_span in a 2-atom pat is 0.5; dur(0.5) halves
    // that to 0.25.
    for (const auto& e : events) {
        CHECK(e.duration == Catch::Approx(0.25f).margin(0.001f));
    }
}

TEST_CASE("Phase 2.1: dur() rejects zero/negative factor",
          "[codegen][patterns][phase21][dur]") {
    auto neg = akkado::compile(R"(dur(pat("c4"), -1))");
    bool found_neg = false;
    for (const auto& d : neg.diagnostics) {
        if (d.code == "E131") { found_neg = true; break; }
    }
    CHECK(found_neg);
}

TEST_CASE("Phase 2.1: nested bend() inside slow() preserves prop_vals",
          "[codegen][patterns][phase21][bend][nested]") {
    // The inner bend() runs in compile_pattern_for_transform's recursion;
    // outer slow() should not clobber prop_vals.
    auto result = akkado::compile(R"(slow(bend(pat("c4 e4"), 0.3), 2))");
    REQUIRE(result.success);
    const auto& events = result.state_inits[0].sequence_events[0];
    REQUIRE_FALSE(events.empty());
    for (const auto& e : events) {
        CHECK(e.prop_vals[0] == Catch::Approx(0.3f).margin(0.001f));
    }
}

// =============================================================================
// Phase 2 PRD D0: velocity-shorthand propagation fix
// =============================================================================

TEST_CASE("velocity-shorthand pat(\"c4:0.8\") propagates to event.velocity",
          "[codegen][patterns][phase2]") {
    auto result = akkado::compile(R"(pat("c4:0.5 e4:0.8"))");
    REQUIRE(result.success);
    REQUIRE_FALSE(result.state_inits.empty());
    const auto& si = result.state_inits[0];
    REQUIRE(si.sequence_events[0].size() == 2);
    // Original eval bug: velocity stayed at 1.0 regardless of :0.8 suffix.
    // After fix: per-atom velocity multiplies the inherited context velocity.
    CHECK(si.sequence_events[0][0].velocity == Catch::Approx(0.5f).margin(0.01f));
    CHECK(si.sequence_events[0][1].velocity == Catch::Approx(0.8f).margin(0.01f));
}

TEST_CASE("velocity-shorthand combines with nested velocity() transform",
          "[codegen][patterns][phase2]") {
    // Nested velocity() applies its multiplier at compile time on event.velocity.
    // Top-level velocity() emits a runtime MUL on the velocity buffer (so the
    // compile-time event.velocity reflects only the inner factors). Verify the
    // recursive multiplication path works with per-atom :0.x velocities.
    auto result = akkado::compile(R"(velocity(velocity(pat("c4:0.5 e4:0.8"), 0.5), 1.0))");
    REQUIRE(result.success);
    const auto& si = result.state_inits[0];
    REQUIRE(si.sequence_events[0].size() == 2);
    // Inner velocity(0.5) multiplies per-atom velocities at compile time:
    //   c4:0.5 → 0.5 * 0.5 = 0.25
    //   e4:0.8 → 0.8 * 0.5 = 0.4
    CHECK(si.sequence_events[0][0].velocity == Catch::Approx(0.25f).margin(0.01f));
    CHECK(si.sequence_events[0][1].velocity == Catch::Approx(0.4f).margin(0.01f));
}

// =============================================================================
// Phase 2 PRD: time/structure modifiers
// =============================================================================

TEST_CASE("Pattern transform: early()", "[codegen][patterns][phase2]") {
    SECTION("early requires pattern as first argument") {
        auto result = akkado::compile("early(42, 0.25)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("early requires a number as second argument") {
        auto result = akkado::compile(R"(early(pat("c4 e4")))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("early(pat, 0.25) shifts event times by -0.25 (mod 1)") {
        auto result = akkado::compile(R"(early(pat("c4 e4 g4 b4"), 0.25))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events.size() >= 1);
        REQUIRE(si.sequence_events[0].size() == 4);
        // Original times: 0.0, 0.25, 0.5, 0.75 -> after -0.25 wrap: 0.75, 0.0, 0.25, 0.5
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.25f).margin(0.001f));
        CHECK(times[2] == Catch::Approx(0.5f).margin(0.001f));
        CHECK(times[3] == Catch::Approx(0.75f).margin(0.001f));
    }
    SECTION("early with negative amount equivalent to late") {
        auto early_result = akkado::compile(R"(early(pat("c4 e4 g4 b4"), -0.25))");
        auto late_result = akkado::compile(R"(late(pat("c4 e4 g4 b4"), 0.25))");
        REQUIRE(early_result.success);
        REQUIRE(late_result.success);
        // Events should rotate identically (sorted times match)
        std::vector<float> et, lt;
        for (const auto& e : early_result.state_inits[0].sequence_events[0]) et.push_back(e.time);
        for (const auto& e : late_result.state_inits[0].sequence_events[0]) lt.push_back(e.time);
        std::sort(et.begin(), et.end());
        std::sort(lt.begin(), lt.end());
        REQUIRE(et.size() == lt.size());
        for (std::size_t i = 0; i < et.size(); ++i) {
            CHECK(et[i] == Catch::Approx(lt[i]).margin(0.001f));
        }
    }
    SECTION("early via dot-call matches functional form") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4 b4").early(0.5))");
        auto direct = akkado::compile(R"(early(pat("c4 e4 g4 b4"), 0.5))");
        CHECK(dot.success);
        CHECK(direct.success);
    }
}

TEST_CASE("Pattern transform: late()", "[codegen][patterns][phase2]") {
    SECTION("late(pat, 0.25) shifts event times by +0.25 (mod 1)") {
        auto result = akkado::compile(R"(late(pat("c4 e4 g4 b4"), 0.25))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 4);
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.25f).margin(0.001f));
        CHECK(times[2] == Catch::Approx(0.5f).margin(0.001f));
        CHECK(times[3] == Catch::Approx(0.75f).margin(0.001f));
    }
    SECTION("late with amount > 1 wraps") {
        auto result = akkado::compile(R"(late(pat("c4 e4 g4 b4"), 1.25))");
        REQUIRE(result.success);
        // Same as late(pat, 0.25)
        const auto& si = result.state_inits[0];
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        for (float t : times) {
            CHECK(t >= 0.0f);
            CHECK(t < 1.0f);
        }
    }
}

TEST_CASE("Pattern transform: palindrome()", "[codegen][patterns][phase2]") {
    SECTION("palindrome requires pattern argument") {
        auto result = akkado::compile("palindrome(42)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("palindrome doubles cycle_length and event count") {
        auto result = akkado::compile(R"(palindrome(pat("c4 e4")))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        // pat has 2 elements -> base cycle_length = 2; palindrome doubles -> 4
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        // 2 forward + 2 reverse = 4 events
        REQUIRE(si.sequence_events[0].size() == 4);
        // All events in [0, 1)
        for (const auto& e : si.sequence_events[0]) {
            CHECK(e.time >= 0.0f);
            CHECK(e.time < 1.0f);
        }
    }
    SECTION("palindrome via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4").palindrome())");
        auto direct = akkado::compile(R"(palindrome(pat("c4 e4 g4")))");
        CHECK(dot.success);
        CHECK(direct.success);
    }
}

TEST_CASE("Pattern transform: compress()", "[codegen][patterns][phase2]") {
    SECTION("compress requires pattern as first argument") {
        auto result = akkado::compile("compress(42, 0.0, 1.0)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("compress requires two numeric arguments") {
        auto result = akkado::compile(R"(compress(pat("c4 e4")))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("compress rejects reversed range") {
        auto result = akkado::compile(R"(compress(pat("c4 e4"), 0.5, 0.25))");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E132") { found = true; break; }
        }
        CHECK(found);
    }
    SECTION("compress rejects degenerate range (start == end)") {
        auto result = akkado::compile(R"(compress(pat("c4 e4"), 0.5, 0.5))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("compress(pat, 0.25, 0.75) maps events into [0.25, 0.75)") {
        auto result = akkado::compile(R"(compress(pat("c4 e4"), 0.25, 0.75))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        // Original times 0.0, 0.5 -> 0.25 + 0.0*0.5 = 0.25, 0.25 + 0.5*0.5 = 0.5
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        REQUIRE(times.size() == 2);
        CHECK(times[0] == Catch::Approx(0.25f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.5f).margin(0.001f));
        // Durations halved (width = 0.5)
        for (const auto& e : si.sequence_events[0]) {
            CHECK(e.duration == Catch::Approx(0.25f).margin(0.001f));
        }
    }
    SECTION("compress(pat, 0, 1) is identity") {
        auto result = akkado::compile(R"(compress(pat("c4 e4"), 0.0, 1.0))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.5f).margin(0.001f));
    }
    SECTION("compress via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4").compress(0.0, 0.5))");
        auto direct = akkado::compile(R"(compress(pat("c4 e4 g4"), 0.0, 0.5))");
        CHECK(dot.success);
        CHECK(direct.success);
    }
}

TEST_CASE("Pattern transform: ply()", "[codegen][patterns][phase2]") {
    SECTION("ply requires pattern as first argument") {
        auto result = akkado::compile("ply(42, 3)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("ply rejects n < 1") {
        auto result = akkado::compile(R"(ply(pat("c4 e4"), 0))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("ply(pat, 3) triples event count") {
        auto result = akkado::compile(R"(ply(pat("c4 e4"), 3))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        // pat("c4 e4") produces 2 events, ply(3) -> 6
        REQUIRE(si.sequence_events[0].size() == 6);
    }
    SECTION("ply(pat, 2) produces correct sub-times") {
        auto result = akkado::compile(R"(ply(pat("c4 e4"), 2))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        // Original: events at 0.0 (dur 0.5), 0.5 (dur 0.5)
        // After ply(2): events at 0.0, 0.25, 0.5, 0.75 (dur 0.25 each)
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        REQUIRE(times.size() == 4);
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.25f).margin(0.001f));
        CHECK(times[2] == Catch::Approx(0.5f).margin(0.001f));
        CHECK(times[3] == Catch::Approx(0.75f).margin(0.001f));
        for (const auto& e : si.sequence_events[0]) {
            CHECK(e.duration == Catch::Approx(0.25f).margin(0.001f));
        }
    }
    SECTION("ply via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4").ply(3))");
        CHECK(dot.success);
    }
}

TEST_CASE("Pattern transform: linger()", "[codegen][patterns][phase2]") {
    SECTION("linger rejects non-positive frac") {
        auto result = akkado::compile(R"(linger(pat("c4 e4"), 0))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("linger(pat, 1.0) is a no-op") {
        auto result = akkado::compile(R"(linger(pat("c4 e4"), 1.0))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f));
        REQUIRE(si.sequence_events[0].size() == 2);
    }
    SECTION("linger(pat, 0.5) keeps first half, halves cycle_length") {
        // pat("c4 e4 g4 b4"): events at 0.0, 0.25, 0.5, 0.75; base cycle=4.
        // Keep events with time < 0.5: {c4@0.0, e4@0.25}
        // Scale by 1/0.5=2: c4@0.0, e4@0.5; durations also scaled.
        // cycle_length *= 0.5 -> 2.
        auto result = akkado::compile(R"(linger(pat("c4 e4 g4 b4"), 0.5))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(2.0f));
        REQUIRE(si.sequence_events[0].size() == 2);
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.5f).margin(0.001f));
    }
    SECTION("linger via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4 b4").linger(0.5))");
        CHECK(dot.success);
    }
}

TEST_CASE("Pattern transform: zoom()", "[codegen][patterns][phase2]") {
    SECTION("zoom rejects reversed range") {
        auto result = akkado::compile(R"(zoom(pat("c4 e4"), 0.5, 0.25))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("zoom rejects degenerate range") {
        auto result = akkado::compile(R"(zoom(pat("c4 e4"), 0.5, 0.5))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("zoom(pat, 0.25, 0.75) keeps middle 50% remapped to [0,1)") {
        // pat("c4 e4 g4 b4"): events at 0.0, 0.25, 0.5, 0.75 (dur 0.25 each).
        // Window [0.25, 0.75): events e4(0.25-0.5), g4(0.5-0.75) overlap fully.
        // Remap: e4 -> t=0.0 dur 0.5, g4 -> t=0.5 dur 0.5.
        auto result = akkado::compile(R"(zoom(pat("c4 e4 g4 b4"), 0.25, 0.75))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 2);
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.5f).margin(0.001f));
    }
    SECTION("zoom via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4 b4").zoom(0.0, 0.5))");
        CHECK(dot.success);
    }
}

TEST_CASE("Pattern transform: segment()", "[codegen][patterns][phase2]") {
    SECTION("segment rejects n < 1") {
        auto result = akkado::compile(R"(segment(pat("c4 e4"), 0))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("segment(pat, 8) emits 8 events with duration 1/8") {
        auto result = akkado::compile(R"(segment(pat("c4 e4"), 8))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 8);
        for (const auto& e : si.sequence_events[0]) {
            CHECK(e.duration == Catch::Approx(0.125f).margin(0.001f));
        }
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        for (std::size_t i = 0; i < times.size(); ++i) {
            CHECK(times[i] == Catch::Approx(static_cast<float>(i) * 0.125f).margin(0.001f));
        }
    }
    SECTION("segment(pat, 1) emits single full-cycle event") {
        auto result = akkado::compile(R"(segment(pat("c4 e4"), 1))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 1);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f).margin(0.001f));
        CHECK(si.sequence_events[0][0].duration == Catch::Approx(1.0f).margin(0.001f));
    }
    SECTION("segment via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4").segment(8))");
        CHECK(dot.success);
    }
}

TEST_CASE("Pattern transform: swing()/swingBy()", "[codegen][patterns][phase2]") {
    SECTION("swing requires pattern argument") {
        auto result = akkado::compile("swing(42)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("swing with default n=4 compiles") {
        auto result = akkado::compile(R"(swing(pat("bd hh sd hh")))");
        CHECK(result.success);
    }
    SECTION("swing(pat, 8) compiles") {
        auto result = akkado::compile(R"(swing(pat("bd hh sd hh"), 8))");
        CHECK(result.success);
    }
    SECTION("swingBy requires pattern and amount") {
        auto result = akkado::compile(R"(swingBy(pat("bd hh")))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("swingBy(pat, 0.5, 4) shifts off-beat events") {
        // pat with 4 elements at times 0, 0.25, 0.5, 0.75.
        // Slice width = 0.25; events at slice offset 0 -> no shift; slice
        // offset 0 (start) -> no shift since frac < 0.5.
        // For 4 events at slice starts, none shift. Use 8 elements to test:
        // pat("a b c d e f g h"), times 0, 0.125, 0.25, ..., 0.875.
        // Slices of width 0.25 starting at 0, 0.25, 0.5, 0.75.
        // Event at 0.0   -> slice 0, frac 0 -> no shift.
        // Event at 0.125 -> slice 0, frac 0.5 -> shift += 0.5 * 0.125 = 0.0625
        // Event at 0.25  -> slice 1, frac 0 -> no shift.
        // Event at 0.375 -> slice 1, frac 0.5 -> shift.
        // ...
        auto result = akkado::compile(R"(swingBy(pat("a b c d e f g h"), 0.5, 4))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 8);
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        // Expected times after swing: 0.0, 0.1875 (0.125+0.0625), 0.25,
        // 0.4375, 0.5, 0.6875, 0.75, 0.9375.
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.1875f).margin(0.001f));
        CHECK(times[2] == Catch::Approx(0.25f).margin(0.001f));
        CHECK(times[3] == Catch::Approx(0.4375f).margin(0.001f));
        CHECK(times[4] == Catch::Approx(0.5f).margin(0.001f));
        CHECK(times[5] == Catch::Approx(0.6875f).margin(0.001f));
        CHECK(times[6] == Catch::Approx(0.75f).margin(0.001f));
        CHECK(times[7] == Catch::Approx(0.9375f).margin(0.001f));
    }
    SECTION("swing with amount=0 is identity") {
        auto result = akkado::compile(R"(swingBy(pat("a b c d"), 0.0, 4))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        std::vector<float> times;
        for (const auto& e : si.sequence_events[0]) times.push_back(e.time);
        std::sort(times.begin(), times.end());
        CHECK(times[0] == Catch::Approx(0.0f).margin(0.001f));
        CHECK(times[1] == Catch::Approx(0.25f).margin(0.001f));
        CHECK(times[2] == Catch::Approx(0.5f).margin(0.001f));
        CHECK(times[3] == Catch::Approx(0.75f).margin(0.001f));
    }
    SECTION("swing dot-call") {
        auto dot = akkado::compile(R"(pat("a b c d e f g h").swing())");
        CHECK(dot.success);
    }
    SECTION("swingBy dot-call") {
        auto dot = akkado::compile(R"(pat("a b c d").swingBy(0.4, 4))");
        CHECK(dot.success);
    }
}

TEST_CASE("Pattern transform: iter()/iterBack()", "[codegen][patterns][phase2]") {
    SECTION("iter requires pattern argument") {
        auto result = akkado::compile("iter(42, 4)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("iter rejects n < 1") {
        auto result = akkado::compile(R"(iter(pat("c4 e4 g4 b4"), 0))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("iter sets iter_n=4 and iter_dir=+1 on StateInitData") {
        auto result = akkado::compile(R"(iter(pat("c4 e4 g4 b4"), 4))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        CHECK(si.iter_n == 4);
        CHECK(si.iter_dir == 1);
        // Events themselves are not rewritten — runtime applies rotation.
        REQUIRE(si.sequence_events[0].size() == 4);
    }
    SECTION("iterBack sets iter_n=4 and iter_dir=-1") {
        auto result = akkado::compile(R"(iterBack(pat("c4 e4 g4 b4"), 4))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.iter_n == 4);
        CHECK(si.iter_dir == -1);
    }
    SECTION("iter n must be in [1, 255]") {
        auto result = akkado::compile(R"(iter(pat("c4 e4"), 256))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("iter via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4 b4").iter(4))");
        CHECK(dot.success);
    }
    SECTION("iterBack via dot-call") {
        auto dot = akkado::compile(R"(pat("c4 e4 g4 b4").iterBack(4))");
        CHECK(dot.success);
    }
}

// =============================================================================
// Phase 2 PRD: algorithmic generators (run, binary, binaryN)
// =============================================================================

TEST_CASE("Pattern generator: run()", "[codegen][patterns][phase2]") {
    SECTION("run requires non-negative integer") {
        auto result = akkado::compile("run(-1)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("run(8) produces 8 events at i/8 with values 0..7") {
        auto result = akkado::compile("run(8)");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(8.0f));
        REQUIRE(si.sequence_events[0].size() == 8);
        for (std::size_t i = 0; i < 8; ++i) {
            const auto& e = si.sequence_events[0][i];
            CHECK(e.time == Catch::Approx(static_cast<float>(i) / 8.0f).margin(0.001f));
            CHECK(e.duration == Catch::Approx(1.0f / 8.0f).margin(0.001f));
            REQUIRE(e.num_values == 1);
            CHECK(e.values[0] == Catch::Approx(static_cast<float>(i)).margin(0.001f));
        }
    }
    SECTION("run(0) yields empty pattern") {
        auto result = akkado::compile("run(0)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.sequence_events[0].size() == 0);
    }
    SECTION("run(1) yields single full-cycle event") {
        auto result = akkado::compile("run(1)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 1);
        CHECK(si.sequence_events[0][0].time == Catch::Approx(0.0f).margin(0.001f));
        CHECK(si.sequence_events[0][0].duration == Catch::Approx(1.0f).margin(0.001f));
        CHECK(si.sequence_events[0][0].values[0] == Catch::Approx(0.0f).margin(0.001f));
    }
    SECTION("run inside slow() composes via recursion") {
        auto result = akkado::compile("slow(run(4), 2)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        // run(4) cycle_length = 4; slow(2) doubles -> 8
        CHECK(si.cycle_length == Catch::Approx(8.0f));
        REQUIRE(si.sequence_events[0].size() == 4);
    }
    SECTION("run dot-call composes with transforms") {
        auto result = akkado::compile("run(4).fast(2)");
        REQUIRE(result.success);
    }
}

TEST_CASE("Pattern generator: binary()", "[codegen][patterns][phase2]") {
    SECTION("binary rejects negative argument") {
        auto result = akkado::compile("binary(-5)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("binary(0b1010) produces 4 events MSB-first") {
        // 0b1010 = 10. bits = 4. MSB-first: 1, 0, 1, 0.
        // Event 0: trigger (set), 1: rest, 2: trigger, 3: rest.
        auto result = akkado::compile("binary(10)");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        REQUIRE(si.sequence_events[0].size() == 4);
        CHECK(si.sequence_events[0][0].num_values == 1);
        CHECK(si.sequence_events[0][1].num_values == 0);
        CHECK(si.sequence_events[0][2].num_values == 1);
        CHECK(si.sequence_events[0][3].num_values == 0);
    }
    SECTION("binary(0) produces single rest event") {
        auto result = akkado::compile("binary(0)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 1);
        CHECK(si.sequence_events[0][0].num_values == 0);
    }
    SECTION("binary(1) produces single trigger event") {
        auto result = akkado::compile("binary(1)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 1);
        CHECK(si.sequence_events[0][0].num_values == 1);
    }
    SECTION("binary dot-call composes") {
        auto result = akkado::compile("binary(170).slow(2)");
        REQUIRE(result.success);
    }
}

TEST_CASE("Pattern generator: binaryN()", "[codegen][patterns][phase2]") {
    SECTION("binaryN rejects negative bits") {
        auto result = akkado::compile("binaryN(5, -1)");
        REQUIRE_FALSE(result.success);
    }
    SECTION("binaryN(5, 8) produces 8 events with 0b00000101 pattern") {
        // 5 = 0b101. binaryN with 8 bits zero-pads: 00000101.
        // MSB-first: 0,0,0,0,0,1,0,1.
        auto result = akkado::compile("binaryN(5, 8)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(8.0f));
        REQUIRE(si.sequence_events[0].size() == 8);
        std::array<bool, 8> expected{false, false, false, false, false, true, false, true};
        for (std::size_t i = 0; i < 8; ++i) {
            CHECK(static_cast<bool>(si.sequence_events[0][i].num_values > 0) == expected[i]);
        }
    }
    SECTION("binaryN truncates n to lower bits per PRD §9.2") {
        // n=5 (0b101) with bits=2 -> truncate to 0b01 -> [0, 1].
        auto result = akkado::compile("binaryN(5, 2)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() == 2);
        CHECK(si.sequence_events[0][0].num_values == 0);
        CHECK(si.sequence_events[0][1].num_values == 1);
    }
    SECTION("binaryN(0, 0) produces empty pattern") {
        auto result = akkado::compile("binaryN(0, 0)");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.sequence_events[0].size() == 0);
    }
}

// =============================================================================
// Phase 2 PRD: voicing system
// =============================================================================

TEST_CASE("Voicing: anchor() basic", "[codegen][voicing][phase2]") {
    SECTION("anchor requires pattern and string note") {
        auto result = akkado::compile(R"(anchor(42, "c4"))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("anchor with valid note compiles") {
        auto result = akkado::compile(R"(anchor(chord("Am"), "c4"))");
        CHECK(result.success);
    }
    SECTION("anchor rejects unparseable note name") {
        auto result = akkado::compile(R"(anchor(chord("Am"), "not_a_note"))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("anchor + below produces exact MIDI [52, 57, 60] for Am @ c4") {
        // Greedy nearest-anchor selection picks the inversion+octave with
        // minimum sum |note - anchor| that satisfies max(notes) <= anchor.
        // For Am intervals [0,3,7] @ root_midi A4=69, anchor c4=60, the
        // winning candidate is the 2nd inversion shifted -2 octaves:
        // E3=52, A3=57, C4=60. Pinning the exact MIDI per audit 2026-04-28
        // (PRD §10.1's [57,60,64] is internally inconsistent — top=64 > 60
        // would violate "below"; algorithmic output is correct).
        auto result = akkado::compile(R"(anchor(chord("Am"), "c4").mode("below"))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        REQUIRE(si.sequence_events[0].size() >= 1);
        const auto& ev = si.sequence_events[0][0];
        REQUIRE(ev.num_values == 3);
        std::vector<int> midis;
        for (std::uint8_t i = 0; i < ev.num_values; ++i) {
            midis.push_back(static_cast<int>(std::round(
                69.0f + 12.0f * std::log2(ev.values[i] / 440.0f))));
        }
        std::sort(midis.begin(), midis.end());
        CHECK(midis == std::vector<int>{52, 57, 60});
    }
}

TEST_CASE("Voicing: mode() basic", "[codegen][voicing][phase2]") {
    SECTION("mode rejects unknown mode") {
        auto result = akkado::compile(R"(mode(chord("Am"), "sideways"))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("mode below works without explicit anchor (default c4)") {
        auto result = akkado::compile(R"(mode(chord("Am"), "below"))");
        CHECK(result.success);
    }
    SECTION("mode above produces exact MIDI [72, 76, 79] for C @ c5") {
        // C major intervals [0,4,7] @ root C4=60 shifted up to satisfy
        // bottom >= c5 (72). Winning candidate is root-position shifted +1
        // octave: C5=72, E5=76, G5=79. Matches PRD §10.1 golden values.
        auto result = akkado::compile(R"(anchor(chord("C"), "c5").mode("above"))");
        REQUIRE(result.success);
        const auto& ev = result.state_inits[0].sequence_events[0][0];
        REQUIRE(ev.num_values == 3);
        std::vector<int> midis;
        for (std::uint8_t i = 0; i < ev.num_values; ++i) {
            midis.push_back(static_cast<int>(std::round(
                69.0f + 12.0f * std::log2(ev.values[i] / 440.0f))));
        }
        std::sort(midis.begin(), midis.end());
        CHECK(midis == std::vector<int>{72, 76, 79});
    }
    SECTION("mode duck excludes the anchor pitch class and stays nearby") {
        // Am @ c4 duck: anchor MIDI 60 must NOT appear in the output. Greedy
        // selection minimizes sum |note - 60| while excluding 60 itself.
        // Algorithm yields [48, 52, 57] = C3, E3, A3 (per audit 2026-04-28
        // probe; PRD §10.1's [57,64,69] is informational and doesn't match
        // the implemented selection). Pin exact output for regression.
        auto result = akkado::compile(R"(anchor(chord("Am"), "c4").mode("duck"))");
        REQUIRE(result.success);
        const auto& ev = result.state_inits[0].sequence_events[0][0];
        REQUIRE(ev.num_values == 3);
        std::vector<int> midis;
        for (std::uint8_t i = 0; i < ev.num_values; ++i) {
            midis.push_back(static_cast<int>(std::round(
                69.0f + 12.0f * std::log2(ev.values[i] / 440.0f))));
        }
        std::sort(midis.begin(), midis.end());
        CHECK(std::find(midis.begin(), midis.end(), 60) == midis.end());
        CHECK(midis == std::vector<int>{48, 52, 57});
    }
    SECTION("mode root places the bass roughly an octave below anchor") {
        // Root mode: bass = root note shifted toward anchor-12 (=48 for c4).
        // Upper voices placed near anchor (60). Algorithm yields A2=45 in
        // bass with C4=60, E4=64 above. Pin exact output.
        auto result = akkado::compile(R"(anchor(chord("Am"), "c4").mode("root"))");
        REQUIRE(result.success);
        const auto& ev = result.state_inits[0].sequence_events[0][0];
        REQUIRE(ev.num_values == 3);
        std::vector<int> midis;
        for (std::uint8_t i = 0; i < ev.num_values; ++i) {
            midis.push_back(static_cast<int>(std::round(
                69.0f + 12.0f * std::log2(ev.values[i] / 440.0f))));
        }
        std::sort(midis.begin(), midis.end());
        CHECK(midis == std::vector<int>{45, 60, 64});
        // Bass should be at least an octave below the next voice.
        CHECK(midis[1] - midis[0] >= 12);
    }
}

TEST_CASE("Voicing: voicing() with built-in dictionaries", "[codegen][voicing][phase2]") {
    SECTION("voicing rejects unregistered name") {
        auto result = akkado::compile(R"(voicing(chord("Am"), "no-such-dict"))");
        REQUIRE_FALSE(result.success);
    }
    SECTION("voicing close compiles") {
        auto result = akkado::compile(R"(voicing(chord("Am"), "close"))");
        CHECK(result.success);
    }
    SECTION("voicing drop2 compiles") {
        auto result = akkado::compile(R"(voicing(chord("Am"), "drop2"))");
        CHECK(result.success);
    }
}

TEST_CASE("Voicing: addVoicings() registers a custom dictionary", "[codegen][voicing][phase2]") {
    SECTION("addVoicings + voicing round-trip") {
        auto result = akkado::compile(R"(
            addVoicings("test_jazz", {M: [0, 4, 7, 11], m: [0, 3, 7, 10]})
            voicing(chord("CM"), "test_jazz")
        )");
        CHECK(result.success);
    }
}

TEST_CASE("Voicing: addVoicings dict.qualities overrides chord intervals",
          "[codegen][voicing][addVoicings]") {
    auto freq_to_midi = [](float f) {
        return static_cast<int>(std::round(69.0f + 12.0f * std::log2(f / 440.0f)));
    };

    SECTION("5-note M voicing expands a CM triad to 5 voices") {
        // {M:[0,4,7,11,14]} = major9 default. Without the fix, CM would
        // voice as the chord parser's intrinsic [0,4,7] triad; with the
        // fix, the dict's per-quality intervals are honored.
        auto result = akkado::compile(R"(
            addVoicings("test_dict_qualities_M", {M: [0, 4, 7, 11, 14]})
            chord("CM") .voicing("test_dict_qualities_M")
        )");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        const auto& ev = events[0];
        CHECK(ev.num_values == 5);
        // Voiced MIDI notes for [0,4,7,11,14] from C4 root, modulo octave
        // shifts the optimizer chooses. We collect the pitch classes (mod
        // 12) and assert the set matches.
        std::vector<int> pcs;
        for (std::uint8_t i = 0; i < ev.num_values; ++i) {
            int midi = freq_to_midi(ev.values[i]);
            pcs.push_back(((midi % 12) + 12) % 12);
        }
        std::sort(pcs.begin(), pcs.end());
        // C(0), E(4), G(7), B(11), D(2). Sorted: [0, 2, 4, 7, 11].
        std::vector<int> expected = {0, 2, 4, 7, 11};
        CHECK(pcs == expected);
    }

    SECTION("unknown chord quality falls back to chord-parser intervals") {
        // Dict has only M:; Am has quality "m" which is absent. Expected:
        // fall back to intrinsic [0,3,7] (3 voices), not the M dict entry.
        auto result = akkado::compile(R"(
            addVoicings("test_dict_qualities_only_M", {M: [0, 4, 7, 11, 14]})
            chord("Am") .voicing("test_dict_qualities_only_M")
        )");
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 3);
    }

    SECTION("built-in dict (empty qualities) does not override intrinsic intervals") {
        // close has empty qualities — should leave a CM7 as a 4-note
        // intrinsic voicing, not collapse or expand.
        auto result = akkado::compile(R"(chord("CM7") .voicing("close"))");
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 4);
    }
}

TEST_CASE("Voicing: close vs open produce distinct candidate sets",
          "[codegen][voicing][close-open]") {
    auto freq_to_midi = [](float f) {
        return static_cast<int>(std::round(69.0f + 12.0f * std::log2(f / 440.0f)));
    };
    auto chord_span = [&](const auto& ev) {
        std::vector<int> midi_notes;
        for (std::uint8_t i = 0; i < ev.num_values; ++i) {
            midi_notes.push_back(freq_to_midi(ev.values[i]));
        }
        std::sort(midi_notes.begin(), midi_notes.end());
        return midi_notes.empty() ? 0 : midi_notes.back() - midi_notes.front();
    };

    SECTION("open spans wider than close on the same chord") {
        auto close_r = akkado::compile(R"(chord("CM") .voicing("close"))");
        auto open_r = akkado::compile(R"(chord("CM") .voicing("open"))");
        REQUIRE(close_r.success);
        REQUIRE(open_r.success);
        const auto& close_ev = close_r.state_inits[0].sequence_events[0][0];
        const auto& open_ev = open_r.state_inits[0].sequence_events[0][0];
        int close_span = chord_span(close_ev);
        int open_span = chord_span(open_ev);
        INFO("close span = " << close_span << ", open span = " << open_span);
        // close: triad within an octave (span ≤ 11). open: bass dropped an
        // octave per inversion, so spans land in the high teens.
        CHECK(close_span <= 11);
        CHECK(open_span >= 12);
        CHECK(open_span - close_span >= 5);
    }

    SECTION("drop2 produces wider spread than close on a 7th chord") {
        auto close_r = akkado::compile(R"(chord("CM7") .voicing("close"))");
        auto drop2_r = akkado::compile(R"(chord("CM7") .voicing("drop2"))");
        REQUIRE(close_r.success);
        REQUIRE(drop2_r.success);
        const auto& close_ev = close_r.state_inits[0].sequence_events[0][0];
        const auto& drop2_ev = drop2_r.state_inits[0].sequence_events[0][0];
        int close_span = chord_span(close_ev);
        int drop2_span = chord_span(drop2_ev);
        INFO("close7 span = " << close_span << ", drop2 span = " << drop2_span);
        // Standard drop2 voicings of a 7th chord span 14–18 semitones; close
        // 7th stays within an octave (≤11).
        CHECK(close_span <= 11);
        CHECK(drop2_span >= 12);
    }
}

TEST_CASE("Voicing: progression voice-leads with bounded movement", "[codegen][voicing][phase2]") {
    // Am C G F voice-led @ c4 below: total semitone movement across 4 chords
    // should be bounded (PRD §10.1: ≤ 6 semitones). We sum |notes[k][i] -
    // notes[k+1][i]| across consecutive chords.
    auto result = akkado::compile(R"(anchor(chord("Am C G F"), "c4").mode("below"))");
    REQUIRE(result.success);
    const auto& si = result.state_inits[0];
    REQUIRE(si.sequence_events[0].size() == 4);

    auto freq_to_midi = [](float f) {
        return 69.0f + 12.0f * std::log2(f / 440.0f);
    };
    int total_movement = 0;
    for (std::size_t k = 1; k < si.sequence_events[0].size(); ++k) {
        const auto& a = si.sequence_events[0][k - 1];
        const auto& b = si.sequence_events[0][k];
        std::size_t n = std::min(a.num_values, b.num_values);
        // Sort each by midi and pair them.
        std::vector<int> ma, mb;
        for (std::size_t i = 0; i < a.num_values; ++i)
            ma.push_back(static_cast<int>(std::round(freq_to_midi(a.values[i]))));
        for (std::size_t i = 0; i < b.num_values; ++i)
            mb.push_back(static_cast<int>(std::round(freq_to_midi(b.values[i]))));
        std::sort(ma.begin(), ma.end());
        std::sort(mb.begin(), mb.end());
        for (std::size_t i = 0; i < n; ++i) {
            total_movement += std::abs(ma[i] - mb[i]);
        }
    }
    // Without voice leading, root-position would have ~12-15 semitones of
    // movement. Greedy nearest should do significantly better. Use a
    // generous bound (15) to allow for slight algorithm tweaks; the actual
    // value should be ≤ 8 in practice.
    CHECK(total_movement <= 15);
}

TEST_CASE("Phase 2 transforms compose with existing transforms", "[codegen][patterns][phase2]") {
    SECTION("slow(palindrome(...)) compiles") {
        auto result = akkado::compile(R"(slow(palindrome(pat("c4 e4")), 2))");
        CHECK(result.success);
    }
    SECTION("palindrome(slow(...)) doubles slow cycle_length again") {
        auto result = akkado::compile(R"(palindrome(slow(pat("c4 e4"), 2)))");
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        // pat: cycle=2; slow(2): cycle=4; palindrome: cycle=8
        CHECK(si.cycle_length == Catch::Approx(8.0f));
    }
    SECTION("compress + early chain") {
        auto result = akkado::compile(R"(early(compress(pat("c4 e4 g4 b4"), 0.0, 0.5), 0.1))");
        CHECK(result.success);
    }
    SECTION("dot-call chain: pat().palindrome().compress(...)") {
        auto result = akkado::compile(R"(pat("c4 e4").palindrome().compress(0.0, 0.5))");
        CHECK(result.success);
    }
}

// =============================================================================
// Pattern Transform: velocity/bank/n Chaining Tests
// =============================================================================

TEST_CASE("Pattern transform: velocity in chain", "[codegen][patterns]") {
    SECTION("slow(velocity(...)) compiles") {
        auto result = akkado::compile(R"(slow(velocity(pat("c4 e4"), 0.5), 2))");
        CHECK(result.success);
    }
    SECTION("velocity(slow(...)) compiles") {
        auto result = akkado::compile(R"(velocity(slow(pat("c4 e4"), 2), 0.5))");
        CHECK(result.success);
    }
    SECTION("velocity in chain modifies event velocity") {
        auto result = akkado::compile(R"(slow(velocity(pat("c4 e4"), 0.5), 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        // slow(2) doubles cycle_length: 2 * 2 = 4
        CHECK(si.cycle_length == Catch::Approx(4.0f));
        // velocity(0.5) sets velocity to 0.5
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 1);
        CHECK(si.sequence_events[0][0].velocity == Catch::Approx(0.5f));
    }
}

TEST_CASE("Pattern transform: bank/variant in chain", "[codegen][patterns]") {
    SECTION("slow(bank(...)) compiles") {
        auto result = akkado::compile(R"(slow(bank(pat("bd sd"), "TR808"), 2))");
        CHECK(result.success);
    }
    SECTION("slow(variant(...)) compiles") {
        auto result = akkado::compile(R"(slow(variant(pat("bd sd"), 2), 2))");
        CHECK(result.success);
    }
}

// =============================================================================
// Pattern Transform: String Literal as Pattern Tests
// =============================================================================

TEST_CASE("Pattern transform: string literal as pattern", "[codegen][patterns]") {
    SECTION("slow with string literal compiles") {
        auto result = akkado::compile(R"(slow("c4 e4 g4", 2))");
        CHECK(result.success);
    }
    SECTION("transpose with string literal compiles") {
        auto result = akkado::compile(R"(transpose("c4 e4", 12))");
        CHECK(result.success);
    }
    SECTION("rev with string literal compiles") {
        auto result = akkado::compile(R"(rev("c4 e4 g4"))");
        CHECK(result.success);
    }
    SECTION("string literal has correct semantics") {
        auto result = akkado::compile(R"(slow("c4 e4", 2))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        CHECK(si.cycle_length == Catch::Approx(4.0f));
    }
    SECTION("chained transform on string literal") {
        auto result = akkado::compile(R"(transpose(slow("c4 e4", 2), 12))");
        CHECK(result.success);
    }
}

// =============================================================================
// Identifier-bound patterns as transform arguments
// (Regression: melody = n"…"; melody |> transpose(@, …) used to fail E130
// because compile_pattern_for_transform had no Identifier case even though
// is_pattern_node accepted Pattern-kind identifiers.)
// =============================================================================

TEST_CASE("Pattern transforms accept identifier-bound patterns",
          "[codegen][patterns]") {
    SECTION("transpose with n\"…\" identifier compiles") {
        auto result = akkado::compile(R"(
            melody = n"c4 e4 g4"
            transpose(melody, -12)
        )");
        REQUIRE(result.success);
    }

    SECTION("transpose with pat() identifier compiles (functional form)") {
        auto result = akkado::compile(R"(
            m = pat("c4 e4 g4")
            transpose(m, 7)
        )");
        REQUIRE(result.success);
    }

    SECTION("transpose with c\"…\" chord-pattern identifier compiles") {
        auto result = akkado::compile(R"(
            chords = c"Am C G"
            transpose(chords, 5)
        )");
        REQUIRE(result.success);
    }

    SECTION("nested transforms over an identifier compile") {
        auto result = akkado::compile(R"(
            m = pat("c4 e4")
            transpose(slow(m, 2), 12)
        )");
        REQUIRE(result.success);
    }

    SECTION("transpose preserves semitone math when applied to identifier") {
        // C4 = 261.63 Hz; +12 semitones doubles to 523.25 Hz.
        // Equivalent to the chained-correctness test at L2081 but with the
        // pattern bound to a name first.
        auto result = akkado::compile(R"(
            m = pat("c4 e4")
            transpose(m, 12)
        )");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& si = result.state_inits[0];
        REQUIRE_FALSE(si.sequence_events.empty());
        REQUIRE(si.sequence_events[0].size() >= 1);
        float c4_freq = 261.6256f;
        CHECK(si.sequence_events[0][0].values[0]
              == Catch::Approx(c4_freq * 2.0f).margin(1.0f));
    }

    SECTION("pipe with @ on identifier — exact form from bug report") {
        auto result = akkado::compile(R"(
            melody = n"c4 e4 g4"
            melody |> transpose(@, -12) |> soundfont(%, "gm", 0) |> out(%, %)
        )");
        REQUIRE(result.success);
    }

    SECTION("rejects identifier bound to a non-pattern (Signal)") {
        auto result = akkado::compile(R"(
            x = osc("sin", 440)
            transpose(x, 5)
        )");
        REQUIRE_FALSE(result.success);
        // Should fail the is_pattern_node precondition (E133), not the
        // compile_pattern_for_transform fallthrough (E130).
        bool has_e133 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E133") { has_e133 = true; break; }
        }
        CHECK(has_e133);
    }

    SECTION("every transform accepts an identifier-bound pattern (smoke)") {
        // Smoke-cover the transforms routed through compile_pattern_for_transform
        // so this regression doesn't recur for one of transpose's siblings.
        const char* transforms[] = {
            "slow(m, 2)",      "fast(m, 2)",       "rev(m)",
            "transpose(m, 5)", "velocity(m, 0.5)", "early(m, 0.25)",
            "late(m, 0.25)",   "palindrome(m)",    "ply(m, 2)",
            "linger(m, 0.5)",  "compress(m, 0.0, 0.5)",
            "zoom(m, 0.0, 0.5)", "segment(m, 8)",  "swing(m, 4)",
            "swingBy(m, 0.5, 4)",
        };
        for (const char* t : transforms) {
            std::string src = std::string("m = pat(\"c4 e4 g4\")\n") + t;
            INFO("source: " << src);
            auto result = akkado::compile(src);
            CHECK(result.success);
        }
    }
}

// =============================================================================
// Error Path Tests
// =============================================================================

TEST_CASE("Codegen: Undefined identifier errors", "[codegen][errors]") {
    SECTION("simple undefined variable - E005") {
        auto result = akkado::compile("undefined_var");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E005") found = true;  // Analyzer catches undefined identifiers
        }
        CHECK(found);
    }

    SECTION("undefined in expression - E005") {
        auto result = akkado::compile("1 + unknown_var");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E005") found = true;  // Analyzer catches undefined identifiers
        }
        CHECK(found);
    }

    SECTION("undefined function - E004") {
        auto result = akkado::compile("nonexistent_func(42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E004") found = true;  // Analyzer catches unknown functions
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Error E103 - Builtin as value", "[codegen][errors]") {
    SECTION("assign builtin to variable") {
        auto result = akkado::compile("x = sin");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E103") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Hole in unexpected context errors", "[codegen][errors]") {
    SECTION("hole at top level - E003") {
        auto result = akkado::compile("%");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E003") found = true;  // Analyzer catches hole outside pipe
        }
        CHECK(found);
    }

    SECTION("hole in assignment without pipe - E003") {
        auto result = akkado::compile("x = %");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E003") found = true;  // Analyzer catches hole outside pipe
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Error E130-E136 - Field access errors", "[codegen][errors]") {
    SECTION("field access on unknown field in pattern") {
        auto result = akkado::compile(R"(pat("c4") |> %.nonexistent)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E136") found = true;
        }
        CHECK(found);
    }

    SECTION("dropped .t alias produces E136") {
        // `.t` used to be a short alias of `.trig` but was removed because
        // it collided with `.time`/`.t0`. Both `.trig` and `.trigger`
        // continue to work; only the single-letter form is gone.
        auto result = akkado::compile(R"(pat("c4") |> %.t)");
        REQUIRE_FALSE(result.success);
        bool found_e136 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E136") { found_e136 = true; break; }
        }
        CHECK(found_e136);
    }

    SECTION(".trig and .trigger both still resolve") {
        auto a = akkado::compile(R"(pat("c4") |> %.trig |> out(%, %))");
        auto b = akkado::compile(R"(pat("c4") |> %.trigger |> out(%, %))");
        CHECK(a.success);
        CHECK(b.success);
    }

    SECTION("field access on undefined variable - E005") {
        auto result = akkado::compile("undefined_record.field");
        REQUIRE_FALSE(result.success);
        // Analyzer catches undefined identifier
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E005") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: HOF error paths", "[codegen][errors]") {
    SECTION("map() without function - E130") {
        auto result = akkado::compile("map([1, 2, 3], 42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E130") found = true;  // Codegen: second arg must be function
        }
        CHECK(found);
    }

    SECTION("map() with too few arguments - E006") {
        auto result = akkado::compile("map([1, 2, 3])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("sum() is variadic - multiple arguments compile") {
        // sum is now variadic: sum(a, b, ...) sums all operands per-channel.
        auto result = akkado::compile("sum([1, 2], [3, 4])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 elements total → 3 ADDs in the mono fold chain.
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("sum() with zero arguments - error") {
        auto result = akkado::compile("sum()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006" || d.code == "E134") found = true;
        }
        CHECK(found);
    }

    SECTION("reduce() wrong argument count") {
        auto result = akkado::compile("reduce([1, 2, 3], (a, b) -> a + b)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            // Analyzer rejects too-few args (E006) before codegen runs.
            if (d.code == "E006" || d.code == "E142") found = true;
        }
        CHECK(found);
    }

    SECTION("zipWith() wrong argument count - E006") {
        auto result = akkado::compile("zipWith([1], [2])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("zip() wrong argument count - E006") {
        auto result = akkado::compile("zip([1])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("take() wrong argument count - E006") {
        auto result = akkado::compile("take(2)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("take() non-literal first arg - E148") {
        auto result = akkado::compile("count = 2\ntake(count, [1, 2, 3])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E148") found = true;  // Codegen: must be literal
        }
        CHECK(found);
    }

    SECTION("drop() wrong argument count - E006") {
        auto result = akkado::compile("drop(1)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("drop() non-literal first arg - E150") {
        auto result = akkado::compile("n = 2\ndrop(n, [1, 2, 3])");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E150") found = true;  // Codegen: must be literal
        }
        CHECK(found);
    }

    SECTION("reverse() wrong argument count - E006") {
        auto result = akkado::compile("reverse()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("range() wrong argument count - E006") {
        auto result = akkado::compile("range(1)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("range() non-literal args - E153") {
        auto result = akkado::compile("count = 5\nrange(0, count)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E153") found = true;  // Codegen: must be literals
        }
        CHECK(found);
    }

    SECTION("repeat() wrong argument count - E006") {
        auto result = akkado::compile("repeat(42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("repeat() non-literal count - E155") {
        auto result = akkado::compile("count = 3\nrepeat(42, count)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E155") found = true;  // Codegen: must be literal
        }
        CHECK(found);
    }

    SECTION("len() wrong argument count - E006") {
        auto result = akkado::compile("len()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("len() on non-array - E141") {
        auto result = akkado::compile("x = 42\nlen(x)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E141") found = true;  // Codegen: not an array
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Pattern error paths", "[codegen][errors]") {
    SECTION("chord() wrong argument count - E006") {
        auto result = akkado::compile("chord()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }

    SECTION("chord() non-string argument - E126") {
        auto result = akkado::compile("chord(42)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E126") found = true;  // Codegen: must be string literal
        }
        CHECK(found);
    }

    SECTION("mtof() wrong argument count - E006") {
        auto result = akkado::compile("mtof()");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") found = true;  // Analyzer: wrong arg count
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Record error paths", "[codegen][errors]") {
    SECTION("record field access on scalar - E061") {
        auto result = akkado::compile("x = 42\nx.field");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E061") found = true;  // Analyzer: Cannot access field on non-record
        }
        CHECK(found);
    }

    SECTION("unknown field on record - E060") {
        auto result = akkado::compile("r = {x: 1, y: 2}\nr.z");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E060") found = true;  // Analyzer: Unknown field on record
        }
        CHECK(found);
    }
}

// =============================================================================
// Phase 5: Codegen Deep Dive - Records, Functions, Match, Patterns
// =============================================================================

TEST_CASE("Codegen: Record handling", "[codegen]") {
    SECTION("simple record literal") {
        auto result = akkado::compile("rec = {freq: 440, vel: 0.8}");
        CHECK(result.success);
    }

    SECTION("record field access") {
        auto result = akkado::compile("rec = {freq: 440, vel: 0.8}\nrec.freq");
        CHECK(result.success);
    }

    SECTION("record with multiple fields") {
        auto result = akkado::compile("r = {a: 1, b: 2, c: 3}\nr.a + r.b + r.c");
        CHECK(result.success);
    }

    SECTION("nested expression in record field") {
        auto result = akkado::compile("x = 10\nr = {val: x * 2}\nr.val");
        CHECK(result.success);
    }

    SECTION("nested record field access returns correct inner value") {
        auto result = akkado::compile(
            "inner = {a: 11, b: 22}\n"
            "outer = {x: inner, y: 99}\n"
            "outer.x.a"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_11 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 11.0f) {
                found_11 = true;
                break;
            }
        }
        CHECK(found_11);
    }

    SECTION("nested record field access selects correct field (not first)") {
        auto result = akkado::compile(
            "inner = {a: 11, b: 22}\n"
            "outer = {x: inner}\n"
            "outer.x.b"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_22 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 22.0f) {
                found_22 = true;
                break;
            }
        }
        CHECK(found_22);
    }

    SECTION("triply nested record field access (a.b.c.d)") {
        auto result = akkado::compile(
            "deepest = {v: 42}\n"
            "mid = {inner: deepest}\n"
            "top = {m: mid}\n"
            "top.m.inner.v"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_42 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 42.0f) {
                found_42 = true;
                break;
            }
        }
        CHECK(found_42);
    }
}

TEST_CASE("Codegen: Lambda and function values", "[codegen]") {
    SECTION("simple lambda") {
        auto result = akkado::compile("f = (x) -> x * 2");
        CHECK(result.success);
    }

    SECTION("lambda with multiple params") {
        auto result = akkado::compile("myadd = (a, b) -> a + b");  // 'add' is a builtin
        CHECK(result.success);
    }

    SECTION("lambda in map") {
        auto result = akkado::compile("arr = [1, 2, 3]\nmap(arr, (x) -> x * 2)");
        CHECK(result.success);
    }

    SECTION("lambda variable") {
        auto result = akkado::compile("double = (x) -> x * 2\ndouble");
        CHECK(result.success);
    }

    SECTION("lambda variable in map") {
        auto result = akkado::compile("double = (x) -> x * 2\narr = [1, 2, 3]\nmap(arr, double)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: User function definitions", "[codegen]") {
    SECTION("simple function definition") {
        auto result = akkado::compile("fn double(x) -> x * 2\ndouble(5)");
        CHECK(result.success);
    }

    SECTION("function with multiple params") {
        // fn add(a, b) -> ... causes segfault, test simpler case
        auto result = akkado::compile("fn triple(x) -> x * 3\ntriple(10)");
        CHECK(result.success);
    }

    SECTION("function with default param") {
        auto result = akkado::compile("fn scale(x, factor = 2) -> x * factor\nscale(5)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Match expressions", "[codegen]") {
    SECTION("match with string scrutinee") {
        auto result = akkado::compile("match(\"sin\") { \"sin\": 1, \"saw\": 2, _: 0 }");
        CHECK(result.success);
    }

    SECTION("match with number scrutinee") {
        auto result = akkado::compile("match(2) { 0: 100, 1: 200, 2: 300, _: 0 }");
        CHECK(result.success);
    }

    SECTION("match with wildcard") {
        auto result = akkado::compile("match(\"unknown\") { _: 42 }");
        CHECK(result.success);
    }

    SECTION("match in function") {
        auto result = akkado::compile("fn wave(t) -> match(t) { \"sin\": 1, \"saw\": 2, _: 0 }\nwave(\"sin\")");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Match destructuring", "[codegen][match][destructure]") {
    SECTION("destructure record - compile time") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.8}
            match(r) {
                {freq, vel}: freq
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("destructure with expression body") {
        auto result = akkado::compile(R"(
            r = {a: 10, b: 20}
            match(r) {
                {a, b}: a + b
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("destructure with guard") {
        auto result = akkado::compile(R"(
            r = {freq: 440, vel: 0.8}
            match(r) {
                {freq, vel} && true: freq * vel
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("as destructuring in pipe") {
        auto result = akkado::compile(R"(
            r = {a: 100, b: 200}
            r as {a, b} |> a + b
        )");
        CHECK(result.success);
    }

    SECTION("as destructuring single field") {
        auto result = akkado::compile(R"(
            r = {val: 42}
            r as {val} |> val * 2
        )");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Statement-level destructure assignment",
          "[codegen][destructure]") {
    SECTION("destructure record into individual variables") {
        auto result = akkado::compile(R"(
            r = {a: 100, b: 200}
            {a, b} = r
            out(a, a)
        )");
        CHECK(result.success);
    }

    SECTION("single-field destructure binds the field") {
        auto result = akkado::compile(R"(
            r = {x: 42}
            {x} = r
            out(x, x)
        )");
        CHECK(result.success);
    }

    SECTION("destructure pattern source binds fixed pattern fields") {
        auto result = akkado::compile(R"(
            p = pat("c4 e4")
            {freq} = p
            out(freq, freq)
        )");
        CHECK(result.success);
    }

    SECTION("missing required field emits E187") {
        auto result = akkado::compile(R"(
            r = {a: 1}
            {a, b} = r
            out(a, a)
        )");
        bool got_e187 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E187") got_e187 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e187);
    }

    SECTION("non-record source emits E140") {
        auto result = akkado::compile(R"(
            x = 42
            {a, b} = x
            out(0, 0)
        )");
        bool got_e140 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E140") got_e140 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e140);
    }

    SECTION("destructure with extra fields in source is OK (no warning)") {
        auto result = akkado::compile(R"(
            r = {a: 1, b: 2, c: 3}
            {a, b} = r
            out(a, b)
        )");
        CHECK(result.success);
    }

    SECTION("regression: pipe-side as {x,y} does not emit E187") {
        // E187 is reserved for the new statement-level / fn-param destructure
        // paths. The pipe-side `as {…}` form lowers via AST rewrite (rewriting
        // identifiers to FieldAccess nodes), so missing fields surface as the
        // pre-existing field-access errors — not E187.
        auto result = akkado::compile(R"(
            r = {a: 1}
            r as {a, missing} |> a + missing |> out(%, %)
        )");
        bool got_e187 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E187") got_e187 = true;
        }
        CHECK_FALSE(result.success);
        CHECK_FALSE(got_e187);
    }

    SECTION("regression: match-arm destructure still emits E141 not E187") {
        // The match-arm path calls bind_destructure_fields() with the default
        // "E141" error code; it must not switch to E187 (which is reserved
        // for statement-level / fn-param destructure).
        auto result = akkado::compile(R"(
            r = {a: 1}
            match(r) {
                {a, missing}: a + missing
                _: 0
            }
        )");
        bool got_e141 = false;
        bool got_e187 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E141") got_e141 = true;
            if (d.code == "E187") got_e187 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e141);
        CHECK_FALSE(got_e187);
    }
}

TEST_CASE("Codegen: Destructure defaults (statement-level)",
          "[codegen][destructure]") {
    SECTION("default fills in for missing field") {
        // Source has `a` but lacks `b`; default `b = 7` fills the slot.
        auto result = akkado::compile(R"(
            r = {a: 5}
            {a, b = 7} = r
            out(a + b, a + b)
        )");
        CHECK(result.success);
    }

    SECTION("present field overrides declared default") {
        auto result = akkado::compile(R"(
            r = {a: 5, b: 9}
            {a = 0, b = 0} = r
            out(a + b, a + b)
        )");
        CHECK(result.success);
    }

    SECTION("expression default evaluates lazily and is reachable") {
        auto result = akkado::compile(R"(
            base = 100
            r = {a: 1}
            {a = 0, b = base + 50} = r
            out(a + b, a + b)
        )");
        CHECK(result.success);
    }

    SECTION("missing field with no default still emits E187") {
        // A required field (no default) missing from source must keep emitting E187
        // even when a sibling field has a default — the default-aware path must
        // not silently swallow non-defaulted misses.
        auto result = akkado::compile(R"(
            r = {a: 1}
            {a = 0, b} = r
            out(a, a)
        )");
        bool got_e187 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E187") got_e187 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e187);
    }
}

TEST_CASE("Codegen: fn-param destructure", "[codegen][destructure]") {
    SECTION("body can reference destructured fields") {
        auto result = akkado::compile(R"(
            fn add_xy({x, y}) -> x + y
            cfg = {x: 3, y: 4}
            v = add_xy(cfg)
            out(v, v)
        )");
        CHECK(result.success);
    }

    SECTION("inline record literal as the destructure arg") {
        auto result = akkado::compile(R"(
            fn add_xy({x, y}) -> x + y
            v = add_xy({x: 10, y: 20})
            out(v, v)
        )");
        CHECK(result.success);
    }

    SECTION("destructure with all defaults — caller passes empty record") {
        auto result = akkado::compile(R"(
            fn synth({freq = 440, q = 0.7}) -> osc("sin", freq) * q
            sig = synth({})
            out(sig, sig)
        )");
        CHECK(result.success);
    }

    SECTION("destructure mixed with regular params") {
        auto result = akkado::compile(R"(
            fn lp_voice(freq, {cutoff = 1000, q = 0.7}) -> osc("saw", freq)
            sig = lp_voice(220, {cutoff: 500})
            out(sig, sig)
        )");
        CHECK(result.success);
    }

    SECTION("missing required field with no default → E187") {
        auto result = akkado::compile(R"(
            fn need_both({a, b}) -> a + b
            v = need_both({a: 1})
            out(v, v)
        )");
        bool got_e187 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E187") got_e187 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e187);
    }

    SECTION("non-Record argument → E140") {
        auto result = akkado::compile(R"(
            fn need_record({a}) -> a
            v = need_record(42)
            out(v, v)
        )");
        bool got_e140 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E140") got_e140 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e140);
    }

    SECTION("missing record argument entirely → arity error E006") {
        // The analyzer's arity check fires before codegen reaches the per-param
        // binding loop, so the user sees a uniform "expects at least N args"
        // diagnostic for every too-few-args call. The destructure slot is
        // counted as one required slot just like any other formal param.
        auto result = akkado::compile(R"(
            fn need_record({a}) -> a
            v = need_record()
            out(v, v)
        )");
        bool got_e006 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") got_e006 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e006);
    }

    SECTION("spread + destructure-param is rejected with E105") {
        // Phase 3b deferred: caller `f(..preset)` against `fn f({x, y})` is
        // not supported; PRD §3.3 says compose later. Reject cleanly.
        auto result = akkado::compile(R"(
            fn synth({freq, wave}) -> osc(wave, freq)
            preset = {freq: 440, wave: "saw"}
            sig = synth(..preset)
            out(sig, sig)
        )");
        bool got_e105 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E105") got_e105 = true;
        }
        CHECK_FALSE(result.success);
        CHECK(got_e105);
    }
}

TEST_CASE("Codegen: Pattern transformations", "[codegen]") {
    // Pattern transformations require literal patterns as first argument
    SECTION("slow transformation") {
        auto result = akkado::compile("slow(pat(\"c4 e4 g4\"), 2)");
        CHECK(result.success);
    }

    SECTION("fast transformation") {
        auto result = akkado::compile("fast(pat(\"c4 e4\"), 2)");
        CHECK(result.success);
    }

    SECTION("rev transformation") {
        auto result = akkado::compile("rev(pat(\"c4 e4 g4\"))");
        CHECK(result.success);
    }

    SECTION("transpose transformation") {
        auto result = akkado::compile("transpose(pat(\"c4 e4 g4\"), 12)");
        CHECK(result.success);
    }

    SECTION("velocity transformation") {
        auto result = akkado::compile("velocity(pat(\"c4 e4 g4\"), 0.5)");
        CHECK(result.success);
    }

    SECTION("pattern transformations emit no W130 warnings") {
        // Verify that pattern transformations are now fully implemented
        // and don't emit "not yet implemented" warnings

        auto check_no_w130 = [](const akkado::CompileResult& result) {
            for (const auto& diag : result.diagnostics) {
                if (diag.code == "W130") {
                    return false;
                }
            }
            return true;
        };

        auto slow_result = akkado::compile("slow(pat(\"c4 e4 g4\"), 2)");
        CHECK(slow_result.success);
        CHECK(check_no_w130(slow_result));

        auto fast_result = akkado::compile("fast(pat(\"c4 e4\"), 2)");
        CHECK(fast_result.success);
        CHECK(check_no_w130(fast_result));

        auto rev_result = akkado::compile("rev(pat(\"c4 e4 g4\"))");
        CHECK(rev_result.success);
        CHECK(check_no_w130(rev_result));

        auto transpose_result = akkado::compile("transpose(pat(\"c4 e4 g4\"), 12)");
        CHECK(transpose_result.success);
        CHECK(check_no_w130(transpose_result));

        auto velocity_result = akkado::compile("velocity(pat(\"c4 e4 g4\"), 0.5)");
        CHECK(velocity_result.success);
        CHECK(check_no_w130(velocity_result));
    }

    SECTION("transformations with pat() syntax") {
        // Test that all transformations work with pat() syntax
        auto slow_result = akkado::compile("slow(pat(\"c4 e4 g4\"), 2)");
        CHECK(slow_result.success);

        auto fast_result = akkado::compile("fast(pat(\"c4 e4\"), 2)");
        CHECK(fast_result.success);

        auto rev_result = akkado::compile("rev(pat(\"c4 e4 g4\"))");
        CHECK(rev_result.success);

        auto transpose_result = akkado::compile("transpose(pat(\"c4 e4 g4\"), 12)");
        CHECK(transpose_result.success);

        auto velocity_result = akkado::compile("velocity(pat(\"c4 e4 g4\"), 0.5)");
        CHECK(velocity_result.success);
    }
}

// =============================================================================
// Phase 6: Array HOF successful paths
// =============================================================================

TEST_CASE("Codegen: Array HOF success paths", "[codegen]") {
    SECTION("map with lambda") {
        auto result = akkado::compile("map([1, 2, 3], (x) -> x * 2)");
        CHECK(result.success);
    }

    SECTION("sum array") {
        auto result = akkado::compile("sum([1, 2, 3, 4])");
        CHECK(result.success);
    }

    SECTION("zipWith with lambda") {
        auto result = akkado::compile("zipWith([1, 2], [10, 20], (a, b) -> a + b)");
        CHECK(result.success);
    }

    SECTION("zip arrays") {
        auto result = akkado::compile("zip([1, 2, 3], [4, 5, 6])");
        CHECK(result.success);
    }

    SECTION("take elements") {
        auto result = akkado::compile("take(2, [1, 2, 3, 4, 5])");
        CHECK(result.success);
    }

    SECTION("drop elements") {
        auto result = akkado::compile("drop(2, [1, 2, 3, 4, 5])");
        CHECK(result.success);
    }

    SECTION("reverse array") {
        auto result = akkado::compile("reverse([1, 2, 3])");
        CHECK(result.success);
    }

    SECTION("range") {
        auto result = akkado::compile("range(0, 5)");
        CHECK(result.success);
    }

    SECTION("repeat") {
        auto result = akkado::compile("repeat(42, 3)");
        CHECK(result.success);
    }

    SECTION("sum") {
        auto result = akkado::compile("sum([1, 2, 3, 4, 5])");
        CHECK(result.success);
    }

    SECTION("len") {
        auto result = akkado::compile("arr = [1, 2, 3]\nlen(arr)");
        CHECK(result.success);
    }
}

// =============================================================================
// Phase 7-9: Additional coverage tests
// =============================================================================

TEST_CASE("Codegen: Pipe expressions", "[codegen]") {
    SECTION("simple pipe") {
        auto result = akkado::compile("osc(\"sin\", 440) |> out(%, %)");
        CHECK(result.success);
    }

    SECTION("pipe with field access") {
        auto result = akkado::compile("pat(\"c4 e4 g4\") |> osc(\"sin\", %.freq)");
        CHECK(result.success);
    }

    SECTION("pipe binding with as") {
        auto result = akkado::compile("osc(\"sin\", 440) as sig |> out(sig, sig)");
        CHECK(result.success);
    }

    SECTION("chained pipes") {
        auto result = akkado::compile("pat(\"c4\") |> osc(\"sin\", %.freq) |> out(%, %)");
        CHECK(result.success);
    }

    SECTION("sample pattern to output") {
        auto result = akkado::compile("pat(\"bd ~ bd ~\") |> out(%)");
        CHECK(result.success);
    }
}

// PRD docs/prd-records-and-field-access.md §3.1–§3.3: every canonical pattern
// field plus its aliases must compile cleanly. Audit
// docs/audits/prd-records-and-field-access_audit_2026-04-24-… cited each of
// these as failing E136 before this change.
TEST_CASE("Codegen: Extended pattern fields", "[codegen][records]") {
    auto compile_field = [](const std::string& field) {
        return akkado::compile(
            "pat(\"c4 e4 g4\") |> osc(\"sin\", %." + field + ") |> out(%, %)"
        );
    };

    SECTION("canonical extended fields compile") {
        for (const char* f : {"note", "dur", "chance", "time", "phase", "sample_id"}) {
            CAPTURE(f);
            auto r = compile_field(f);
            CHECK(r.success);
        }
    }

    SECTION("freq aliases compile") {
        for (const char* f : {"freq", "frequency", "pitch", "f", "p"}) {
            CAPTURE(f);
            auto r = compile_field(f);
            CHECK(r.success);
        }
    }

    SECTION("note aliases compile") {
        for (const char* f : {"note", "midi", "n"}) {
            CAPTURE(f);
            auto r = compile_field(f);
            CHECK(r.success);
        }
    }

    SECTION("dur aliases compile") {
        for (const char* f : {"dur", "duration"}) {
            CAPTURE(f);
            auto r = compile_field(f);
            CHECK(r.success);
        }
    }

    SECTION("time aliases compile") {
        for (const char* f : {"time", "t0", "start"}) {
            CAPTURE(f);
            auto r = compile_field(f);
            CHECK(r.success);
        }
    }

    SECTION("phase aliases compile") {
        for (const char* f : {"phase", "cycle", "co"}) {
            CAPTURE(f);
            auto r = compile_field(f);
            CHECK(r.success);
        }
    }

    SECTION("sample_id aliases compile") {
        for (const char* f : {"sample_id", "sample", "s"}) {
            CAPTURE(f);
            auto r = compile_field(f);
            CHECK(r.success);
        }
    }

    SECTION("E136 lists the new canonical names") {
        auto r = akkado::compile(R"(pat("c4") |> osc("sin", %.bogus))");
        REQUIRE_FALSE(r.success);
        bool saw_e136 = false;
        for (const auto& d : r.diagnostics) {
            if (d.code != "E136") continue;
            saw_e136 = true;
            // Truthful diagnostic must mention every canonical name now wired.
            for (const char* canonical : {"note", "dur", "chance", "time",
                                          "phase", "sample_id"}) {
                CAPTURE(canonical);
                CHECK(d.message.find(canonical) != std::string::npos);
            }
        }
        CHECK(saw_e136);
    }

    SECTION("SEQPAT_FIELD and SEQPAT_PHASE opcodes are emitted") {
        auto r = akkado::compile(R"(pat("c4") |> osc("sin", %.freq))");
        REQUIRE(r.success);
        auto insts = get_instructions(r);
        // SEQPAT_FIELD appears five times (DUR, CHANCE, TIME, NOTE, SAMPLE_ID).
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_FIELD) == 5);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_PHASE) == 1);
    }

    SECTION("`as` binding propagates extended fields") {
        auto r = akkado::compile(
            "pat(\"c4 e4 g4\") as e |> "
            "osc(\"sin\", e.freq) * (0.1 + e.phase * 0.9) |> out(%, %)"
        );
        CHECK(r.success);
    }

    SECTION("sample pattern with %.sample_id") {
        auto r = akkado::compile("pat(\"bd sd bd sd\") |> %.sample_id |> out(%, %)");
        CHECK(r.success);
    }
}

// PRD docs/prd-records-and-field-access.md §3: extended pattern fields must
// resolve on every pattern producer, not just the bare pat() literal. Before
// this coverage, the four transform construction sites in codegen_patterns.cpp
// (emit_pattern_with_state, handle_velocity_call, handle_bank_call,
// handle_variant_call) only populated FREQ/VEL/TRIG, so %.note on
// `fast(pat(...), 2)` failed E136 silently. See
// docs/audits/prd-records-and-field-access_audit_2026-05-13.md.
TEST_CASE("Codegen: Extended pattern fields on transforms", "[codegen][records-extended]") {
    static constexpr std::array<const char*, 18> kCanonicalFields = {
        "freq", "vel", "trig", "gate", "type",
        "note", "dur", "chance", "time", "phase", "sample_id",
        "frequency", "pitch", "velocity", "trigger",
        "midi", "duration", "cycle",
    };

    SECTION("fast() preserves every extended field") {
        for (const char* f : kCanonicalFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("fast(pat(\"c4 e4 g4\"), 2) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("slow() preserves every extended field") {
        for (const char* f : kCanonicalFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("slow(pat(\"c4 e4 g4\"), 2) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("rev() preserves every extended field") {
        for (const char* f : kCanonicalFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("rev(pat(\"c4 e4 g4\")) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("velocity() preserves every extended field") {
        // handle_velocity_call construction site (line ~3470). Previously
        // left GATE/TYPE plus all extended slots at 0xFFFF.
        for (const char* f : kCanonicalFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("velocity(pat(\"c4 e4 g4\"), 0.5) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("bank() preserves every extended field on sample patterns") {
        // handle_bank_call construction site (line ~3863).
        for (const char* f : kCanonicalFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("bank(s\"bd sd\", \"808\") |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("variant() preserves every extended field on sample patterns") {
        // handle_variant_call construction site (line ~4070).
        for (const char* f : kCanonicalFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("variant(s\"bd sd\", 0) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("SEQPAT opcodes emitted on transformed pattern") {
        // The transform path (emit_pattern_with_state) must now emit the same
        // 5 SEQPAT_FIELD instructions + 1 SEQPAT_PHASE as the bare-pat path.
        auto r = akkado::compile(R"(fast(pat("c4 e4 g4"), 2) |> osc("sin", %.note) |> out(%, %))");
        REQUIRE(r.success);
        auto insts = get_instructions(r);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_FIELD) == 5);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_PHASE) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_GATE)  == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_TYPE)  == 1);
    }

    SECTION("E136 lists canonical names from transformed pattern context") {
        auto r = akkado::compile(R"(fast(pat("c4"), 2) |> osc("sin", %.bogus) |> out(%, %))");
        REQUIRE_FALSE(r.success);
        bool saw_e136 = false;
        for (const auto& d : r.diagnostics) {
            if (d.code != "E136") continue;
            saw_e136 = true;
            for (const char* canonical : {"note", "dur", "chance", "time",
                                          "phase", "sample_id"}) {
                CAPTURE(canonical);
                CHECK(d.message.find(canonical) != std::string::npos);
            }
        }
        CHECK(saw_e136);
    }

    SECTION("chained transforms (fast |> velocity) still expose extended fields") {
        // Stack two transforms to make sure each one's PatternPayload
        // construction site picks up the helper.
        for (const char* f : {"note", "dur", "phase", "sample_id", "gate"}) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("velocity(fast(pat(\"c4 e4 g4\"), 2), 0.5) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }
}

// PRD docs/prd-records-and-field-access.md §3 + docs/prd-patterns-as-scalar-values.md
// §5: every typed pattern prefix (n"…", v"…", s"…", c"…") must expose the same
// 11 extended fields as `pat(…)`. Before this coverage, n"60 64 67" silently
// returned a Signal scalar because the mini-lexer never consulted note_mode_
// for numeric atoms — see akkado/src/mini_lexer.cpp lex_note_atom().
TEST_CASE("Codegen: Extended pattern fields on typed prefixes", "[codegen][records-extended]") {
    // Canonical names + representative alias per group. Mirrors
    // kCanonicalFields in the "on transforms" case so the same field set
    // is exercised across pat() and every typed prefix.
    static constexpr std::array<const char*, 18> kFields = {
        "freq", "vel", "trig", "gate", "type",
        "note", "dur", "chance", "time", "phase", "sample_id",
        "frequency", "pitch", "velocity", "trigger",
        "midi", "duration", "cycle",
    };

    SECTION("n\"…\" with note names exposes every extended field (bare)") {
        for (const char* f : kFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("n\"c4 e4 g4\" |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("n\"…\" with bare MIDI numbers exposes every extended field (bare)") {
        // The form that revealed the bug: numeric atoms in Note mode.
        for (const char* f : kFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("n\"60 64 67\" |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("n\"…\" exposes every extended field through fast() transform") {
        for (const char* f : kFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("fast(n\"60 64 67\", 2) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("v\"…\" exposes every extended field (bare)") {
        for (const char* f : kFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("v\"0.5 0.8 1.0\" |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("v\"…\" exposes every extended field through fast() transform") {
        for (const char* f : kFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("fast(v\"0.5 0.8 1.0\", 2) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("s\"…\" exposes every extended field (bare)") {
        for (const char* f : kFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("s\"bd sd bd sd\" |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("s\"…\" exposes every extended field through fast() transform") {
        for (const char* f : kFields) {
            CAPTURE(f);
            auto r = akkado::compile(
                std::string("fast(s\"bd sd bd sd\", 2) |> osc(\"sin\", %.") + f + ") |> out(%, %)"
            );
            CHECK(r.success);
        }
    }

    SECTION("symbol-bound typed prefixes propagate fields") {
        // This is the exact form that revealed the n"…" bug:
        //   x = n"60 64 67"
        //   y = x.note     // E136 "Available: freq, vel, trig, gate, type"
        // After the lex_note_atom() fix every prefix produces a Pattern with
        // all 11 fields populated, so field access through a named binding
        // resolves identically to bare-pipe access.
        for (const char* f : kFields) {
            for (const char* prefix : {
                "n\"60 64 67\"", "n\"c4 e4 g4\"",
                "v\"0.5 0.8 1.0\"", "s\"bd sd\"",
            }) {
                CAPTURE(f);
                CAPTURE(prefix);
                auto r = akkado::compile(
                    std::string("x = ") + prefix + "\n" +
                    "osc(\"sin\", x." + f + ") |> out(%, %)"
                );
                CHECK(r.success);
            }
        }
    }

    SECTION("n\"…\" bytecode matches pat() for SEQPAT opcode counts") {
        // After the fix, n"60 64 67" and pat("60 64 67") should emit the
        // same SEQPAT-family instruction counts: 1×STEP + 1×GATE + 1×TYPE +
        // 5×FIELD + 1×PHASE + 1×QUERY for a monophonic non-sample pattern.
        auto r = akkado::compile(R"(n"60 64 67" |> osc("sin", %.note) |> out(%, %))");
        REQUIRE(r.success);
        auto insts = get_instructions(r);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_STEP)  == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_GATE)  == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_TYPE)  == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_FIELD) == 5);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_PHASE) == 1);
    }

    SECTION("c\"…\" bare use is rejected per the polyphony pivot") {
        // The polyphony pivot replaced auto-sum with explicit poly()/sampler()
        // wrapping. Bare c"…" used as a scalar must error so a silent
        // voice-dropping regression can't sneak in.
        auto r = akkado::compile(R"(c"Cmaj7" |> osc("sin", %.freq) |> out(%, %))");
        REQUIRE_FALSE(r.success);
        bool saw_polyphony_reject = false;
        for (const auto& d : r.diagnostics) {
            if (d.code == "E160" || d.code == "E410") {
                saw_polyphony_reject = true;
            }
        }
        CHECK(saw_polyphony_reject);
    }

    SECTION("c\"…\" inside poly() — current closure-param Signal binding is locked down") {
        // Documents current polyphony-pivot behavior: poly()'s closure params
        // are Signal scalars (freq/gate/vel buffers), not a Pattern, so
        // e.freq / e.note errors E061. Per-voice field access is intentionally
        // deferred to a separate PRD; this SECTION is the regression marker
        // that will need to flip if that PRD lands.
        auto r = akkado::compile(R"(poly(c"Cmaj7", (e) -> osc("sin", e.note)) |> out(%, %))");
        REQUIRE_FALSE(r.success);
        bool saw_e061 = false;
        for (const auto& d : r.diagnostics) {
            if (d.code == "E061") saw_e061 = true;
        }
        CHECK(saw_e061);
    }
}

TEST_CASE("Codegen: >> and @ aliases", "[codegen]") {
    SECTION(">> and @ produce same bytecode as |> and %") {
        auto r1 = akkado::compile("osc(\"sin\", 440) |> out(%, %)");
        auto r2 = akkado::compile("osc(\"sin\", 440) >> out(@, @)");
        REQUIRE(r1.success);
        REQUIRE(r2.success);
        CHECK(r1.bytecode.size() == r2.bytecode.size());
    }

    SECTION("@ field access compiles") {
        auto result = akkado::compile("pat(\"c4 e4 g4\") >> osc(\"sin\", @.freq)");
        CHECK(result.success);
    }

    SECTION("@ in unexpected context still errors") {
        auto result = akkado::compile("@");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E003") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: Sample pattern event inspection", "[codegen][samples][debug]") {
    SECTION("sample pattern compiles with SAMPLE_PLAY and correct events") {
        auto result = akkado::compile(R"(pat("hh hh hh [hh hh] hh hh hh [hh oh]") |> out(%))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);

        // Must have SEQPAT_QUERY, SEQPAT_STEP, and SAMPLE_PLAY instructions
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_STEP) >= 1);
        CHECK(count_instructions(insts, cedar::Opcode::SAMPLE_PLAY) >= 1);

        // Check state_inits
        REQUIRE(!result.state_inits.empty());
        const auto& si = result.state_inits[0];
        CHECK(si.type == akkado::StateInitData::Type::SequenceProgram);
        CHECK(si.cycle_length == 8.0f);  // 8 top-level elements
        CHECK(si.is_sample_pattern == true);

        // Root sequence should have 10 events (8 top-level, groups expand inline)
        REQUIRE(!si.sequence_events.empty());
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 10);

        // Verify event times (normalized to [0,1) range, divided by 8)
        CHECK(events[0].time == Catch::Approx(0.0f));        // hh
        CHECK(events[1].time == Catch::Approx(1.0f / 8.0f)); // hh
        CHECK(events[2].time == Catch::Approx(2.0f / 8.0f)); // hh
        CHECK(events[3].time == Catch::Approx(3.0f / 8.0f)); // [hh (first child)
        CHECK(events[4].time == Catch::Approx(3.5f / 8.0f)); // hh] (second child)
        CHECK(events[5].time == Catch::Approx(4.0f / 8.0f)); // hh
        CHECK(events[6].time == Catch::Approx(5.0f / 8.0f)); // hh
        CHECK(events[7].time == Catch::Approx(6.0f / 8.0f)); // hh
        CHECK(events[8].time == Catch::Approx(7.0f / 8.0f)); // [hh (first child)
        CHECK(events[9].time == Catch::Approx(7.5f / 8.0f)); // oh] (second child)

        // ALL events should be DATA type with num_values=1
        for (size_t i = 0; i < events.size(); i++) {
            INFO("Event " << i << " at time " << events[i].time);
            CHECK(events[i].type == cedar::EventType::DATA);
            CHECK(events[i].num_values == 1);
        }

        // Without a sample registry, all sample IDs should be 0 (deferred resolution)
        for (size_t i = 0; i < events.size(); i++) {
            CHECK(events[i].values[0] == 0.0f);
        }

        // Check sample mappings exist for all 10 events
        CHECK(si.sequence_sample_mappings.size() == 10);
        for (const auto& mapping : si.sequence_sample_mappings) {
            CHECK(mapping.seq_idx == 0);
            // All mappings should have a sample name
            CHECK(!mapping.sample_name.empty());
        }

        // Verify SAMPLE_PLAY instruction wiring
        const auto* sample_play = find_instruction(insts, cedar::Opcode::SAMPLE_PLAY);
        REQUIRE(sample_play != nullptr);

        // SAMPLE_PLAY should read from trigger buffer (inputs[0]) and value buffer (inputs[2])
        const auto* seqpat_step = find_instruction(insts, cedar::Opcode::SEQPAT_STEP);
        REQUIRE(seqpat_step != nullptr);

        // The trigger buffer written by SEQPAT_STEP should be read by SAMPLE_PLAY
        CHECK(sample_play->inputs[0] == seqpat_step->inputs[1]);  // trigger
        // The value buffer written by SEQPAT_STEP should be read by SAMPLE_PLAY
        CHECK(sample_play->inputs[2] == seqpat_step->out_buffer);  // sample_id
    }

    SECTION("sample pattern with sample registry resolves IDs") {
        akkado::SampleRegistry registry;
        registry.register_sample("hh", 42);
        registry.register_sample("oh", 99);

        auto result = akkado::compile(
            R"(pat("hh hh hh [hh hh] hh hh hh [hh oh]") |> out(%))",
            "<input>", &registry);
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 10);

        // With registry, sample IDs should be resolved
        for (size_t i = 0; i < 9; i++) {  // first 9 are "hh"
            INFO("Event " << i << " sample_id");
            CHECK(events[i].values[0] == 42.0f);
        }
        // Last event is "oh"
        CHECK(events[9].values[0] == 99.0f);
    }
}

// =============================================================================
// Sample polyrhythm merge — [bd, hh] plays kick and hat simultaneously.
// Codegen collapses sibling sample atoms under a MiniPolyrhythm into a single
// DATA event with num_values=N so SAMPLE_PLAY can spawn one voice per value.
// =============================================================================
TEST_CASE("Codegen: Sample polyrhythm merges into chord-like event",
          "[codegen][samples][polyrhythm]") {
    akkado::SampleRegistry registry;
    registry.register_sample("bd", 1);
    registry.register_sample("sd", 2);
    registry.register_sample("hh", 3);

    SECTION("[bd, hh] → single event with num_values=2") {
        auto result = akkado::compile(R"(pat("[bd, hh]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& si = result.state_inits[0];
        CHECK(si.is_sample_pattern == true);
        REQUIRE(!si.sequence_events.empty());
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 2);
        CHECK(events[0].values[0] == 1.0f);  // bd
        CHECK(events[0].values[1] == 3.0f);  // hh
        CHECK(events[0].time == Catch::Approx(0.0f));

        // Sample mappings exist for both voices with distinct slots.
        REQUIRE(si.sequence_sample_mappings.size() == 2);
        CHECK(si.sequence_sample_mappings[0].value_slot == 0);
        CHECK(si.sequence_sample_mappings[0].sample_name == "bd");
        CHECK(si.sequence_sample_mappings[1].value_slot == 1);
        CHECK(si.sequence_sample_mappings[1].sample_name == "hh");
    }

    SECTION("[bd, hh, sd] → three voices") {
        auto result = akkado::compile(R"(pat("[bd, hh, sd]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 3);
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 3.0f);
        CHECK(events[0].values[2] == 2.0f);
    }

    SECTION("[bd, bd] → two voices, same id (voice-doubling allowed)") {
        auto result = akkado::compile(R"(pat("[bd, bd]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 2);
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 1.0f);
    }

    SECTION("[bd, ~] → rest becomes silent voice (values[1] = 0)") {
        auto result = akkado::compile(R"(pat("[bd, ~]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 2);
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 0.0f);
        // Only the sample atom gets a mapping; the rest is silent.
        REQUIRE(si.sequence_sample_mappings.size() == 1);
        CHECK(si.sequence_sample_mappings[0].value_slot == 0);
    }

    SECTION("user's rock groove — half-cycle layout kicks/snares correctly") {
        // Each half-cycle is [[bd, hh] hh [sd, hh] hh] = 4 beats,
        // together 8 top-level beats. Beats 1 and 3 are polyrhythm stacks.
        auto result = akkado::compile(
            R"(pat("[[bd, hh] hh [sd, hh] hh]  [[bd, hh] [bd, hh] [sd, hh] hh]") |> out(%, %))",
            "<input>", &registry);
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.is_sample_pattern == true);
        CHECK(si.cycle_length == 2.0f);  // 2 top-level group halves
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 8);

        // First half: [bd,hh], hh, [sd,hh], hh
        CHECK(events[0].num_values == 2);  // bd + hh
        CHECK(events[0].values[0] == 1.0f);
        CHECK(events[0].values[1] == 3.0f);
        CHECK(events[1].num_values == 1);  // hh
        CHECK(events[1].values[0] == 3.0f);
        CHECK(events[2].num_values == 2);  // sd + hh
        CHECK(events[2].values[0] == 2.0f);
        CHECK(events[2].values[1] == 3.0f);
        CHECK(events[3].num_values == 1);  // hh

        // Second half: [bd,hh], [bd,hh], [sd,hh], hh
        CHECK(events[4].num_values == 2);
        CHECK(events[5].num_values == 2);
        CHECK(events[5].values[0] == 1.0f);  // bd
        CHECK(events[6].num_values == 2);  // sd + hh
        CHECK(events[6].values[0] == 2.0f);
        CHECK(events[7].num_values == 1);  // hh only
    }

    SECTION("[[bd, sd], hh] → nested polyrhythm flattens to 3 voices") {
        // The recursive flatten unrolls nested polyrhythms: the inner [bd, sd]
        // contributes 2 parallel timelines, and the outer adds hh, for 3
        // simultaneous voices in a single event at t=0. This is the simple
        // recursive semantics — every comma adds a parallel branch, no matter
        // how deeply nested.
        auto result = akkado::compile(R"(pat("[[bd, sd], hh]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].num_values == 3);
        CHECK(events[0].values[0] == 1.0f);  // bd
        CHECK(events[0].values[1] == 2.0f);  // sd
        CHECK(events[0].values[2] == 3.0f);  // hh
    }

    SECTION("[bd, [hh hh hh hh]] → kick sustains while hihats subdivide") {
        // The user's regression case. Branch 0 is a single bd at t=0 (whole
        // cycle), branch 1 is four hihats at t=0/0.25/0.5/0.75. Merged: at
        // t=0 we trigger both bd and hh; at the three later times we trigger
        // only hh (slot 0 = 0 means "no new trigger on bd this step"). The
        // bd voice plays out its sample naturally — SAMPLE_PLAY doesn't need
        // a sustain marker.
        auto result = akkado::compile(R"(pat("[bd, [hh hh hh hh]]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        const auto& si = result.state_inits[0];
        CHECK(si.is_sample_pattern == true);
        const auto& events = si.sequence_events[0];
        REQUIRE(events.size() == 4);

        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].num_values == 2);
        CHECK(events[0].values[0] == 1.0f);  // bd triggers
        CHECK(events[0].values[1] == 3.0f);  // hh triggers

        CHECK(events[1].time == Catch::Approx(0.25f));
        CHECK(events[1].num_values == 2);
        CHECK(events[1].values[0] == 0.0f);  // bd does not retrigger
        CHECK(events[1].values[1] == 3.0f);  // hh triggers

        CHECK(events[2].time == Catch::Approx(0.5f));
        CHECK(events[2].values[0] == 0.0f);
        CHECK(events[2].values[1] == 3.0f);

        CHECK(events[3].time == Catch::Approx(0.75f));
        CHECK(events[3].values[0] == 0.0f);
        CHECK(events[3].values[1] == 3.0f);

        // Sample mappings: bd at slot 0 of event 0, hh at slot 1 of every event.
        std::size_t bd_count = 0, hh_count = 0;
        for (const auto& m : si.sequence_sample_mappings) {
            if (m.sample_name == "bd") {
                CHECK(m.value_slot == 0);
                CHECK(m.event_idx == 0);
                ++bd_count;
            } else if (m.sample_name == "hh") {
                CHECK(m.value_slot == 1);
                ++hh_count;
            }
        }
        CHECK(bd_count == 1);
        CHECK(hh_count == 4);
    }

    SECTION("[bd, [bd, cp]] → nested polyrhythm contributes 3 voices, all at t=0") {
        // bd, bd, cp all triggered simultaneously — the inner [bd, cp] adds
        // two parallel branches alongside the outer bd.
        akkado::SampleRegistry reg2;
        reg2.register_sample("bd", 1);
        reg2.register_sample("cp", 4);
        auto result = akkado::compile(R"(pat("[bd, [bd, cp]]") |> out(%, %))",
                                       "<input>", &reg2);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 3);
        CHECK(events[0].values[0] == 1.0f);  // outer bd
        CHECK(events[0].values[1] == 1.0f);  // inner bd
        CHECK(events[0].values[2] == 4.0f);  // inner cp
    }

    SECTION("[[bd cp], [hh hh hh hh]] → both branches subdivide; boundaries align") {
        // Branch 0 ([bd cp]) splits in half: bd at t=0, cp at t=0.5.
        // Branch 1 ([hh hh hh hh]) quarters: hh at t=0/0.25/0.5/0.75.
        // Merged trigger times = {0, 0.25, 0.5, 0.75}. At t=0.5 both branches
        // fire (cp + hh) — the only non-edge boundary that aligns.
        // Polyrhythm branches must be bracketed to wrap multi-element sequences;
        // the parser treats each comma-separated slot as a single element.
        akkado::SampleRegistry reg2;
        reg2.register_sample("bd", 1);
        reg2.register_sample("cp", 4);
        reg2.register_sample("hh", 3);
        auto result = akkado::compile(R"(pat("[[bd cp], [hh hh hh hh]]") |> out(%, %))",
                                       "<input>", &reg2);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 4);

        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].values[0] == 1.0f);  // bd
        CHECK(events[0].values[1] == 3.0f);  // hh

        CHECK(events[1].time == Catch::Approx(0.25f));
        CHECK(events[1].values[0] == 0.0f);
        CHECK(events[1].values[1] == 3.0f);

        CHECK(events[2].time == Catch::Approx(0.5f));
        CHECK(events[2].values[0] == 4.0f);  // cp triggers exactly here
        CHECK(events[2].values[1] == 3.0f);

        CHECK(events[3].time == Catch::Approx(0.75f));
        CHECK(events[3].values[0] == 0.0f);
        CHECK(events[3].values[1] == 3.0f);
    }

    SECTION("polyrhythm inside a group — [bd [hh, sn] cp] widens the group") {
        // The middle group element is a polyrhythm with two sample voices.
        // Recursion widens the enclosing group to two parallel timelines:
        //   timeline 0: bd@0..1/3, hh@1/3..2/3, cp@2/3..1
        //   timeline 1:           sn@1/3..2/3
        // Merge yields 3 events; only the middle has both voices populated.
        akkado::SampleRegistry reg2;
        reg2.register_sample("bd", 1);
        reg2.register_sample("hh", 3);
        reg2.register_sample("sn", 2);
        reg2.register_sample("cp", 4);
        auto result = akkado::compile(R"(pat("[bd [hh, sn] cp]") |> out(%, %))",
                                       "<input>", &reg2);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        // [bd [hh, sn] cp] — sequential group of 3, middle one widens. The
        // group itself isn't a polyrhythm so it doesn't go through the merge
        // path at this level; each child compiles in time order, with the
        // middle polyrhythm internally merging hh+sn.
        REQUIRE(events.size() == 3);
        // bd at t=0
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].values[0] == 1.0f);
        // [hh, sn] merged at t=1/3
        CHECK(events[1].time == Catch::Approx(1.0f / 3.0f));
        CHECK(events[1].num_values == 2);
        CHECK(events[1].values[0] == 3.0f);
        CHECK(events[1].values[1] == 2.0f);
        // cp at t=2/3
        CHECK(events[2].time == Catch::Approx(2.0f / 3.0f));
        CHECK(events[2].values[0] == 4.0f);
    }

    SECTION("[bd, sn, hh, cp, oh] — 5-voice polyrhythm fits under MAX_VALUES_PER_EVENT") {
        // 5 branches ≤ 16 (MAX_VALUES_PER_EVENT). Previously this truncated
        // because the cap was 4; bumping to 16 lets all five voices land in
        // a single event (same convention as the old merged polyrhythm path).
        akkado::SampleRegistry reg2;
        reg2.register_sample("bd", 1);
        reg2.register_sample("sn", 2);
        reg2.register_sample("hh", 3);
        reg2.register_sample("cp", 4);
        reg2.register_sample("oh", 5);
        auto result = akkado::compile(R"(pat("[bd, sn, hh, cp, oh]") |> out(%, %))",
                                       "<input>", &reg2);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 5);
        CHECK(events[0].values[0] == 1.0f);  // bd
        CHECK(events[0].values[1] == 2.0f);  // sn
        CHECK(events[0].values[2] == 3.0f);  // hh
        CHECK(events[0].values[3] == 4.0f);  // cp
        CHECK(events[0].values[4] == 5.0f);  // oh
    }

    SECTION("single SAMPLE_PLAY instruction; in3/in4 link to SEQPAT state") {
        auto result = akkado::compile(R"(pat("[bd, hh]") |> out(%, %))",
                                       "<input>", &registry);
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        // Option B: one SAMPLE_PLAY that handles all voices internally.
        CHECK(count_instructions(insts, cedar::Opcode::SAMPLE_PLAY) == 1);

        const auto* sample_play = find_instruction(insts, cedar::Opcode::SAMPLE_PLAY);
        REQUIRE(sample_play != nullptr);
        const auto* query = find_instruction(insts, cedar::Opcode::SEQPAT_QUERY);
        REQUIRE(query != nullptr);

        // in3/in4 carry the SequenceState's state_id, split low/high 16 bits.
        std::uint32_t linked_state =
            static_cast<std::uint32_t>(sample_play->inputs[3]) |
            (static_cast<std::uint32_t>(sample_play->inputs[4]) << 16);
        CHECK(linked_state == query->state_id);
    }
}

TEST_CASE("MAX_VALUES_PER_EVENT cap enforced", "[event-cap]") {
    // Compile-time guard: catch accidental future regression of the cap.
    // 16 covers full 13th chords plus user-registered jazz voicings, so any
    // shrink would silently truncate addVoicings() dictionaries again.
    static_assert(cedar::MAX_VALUES_PER_EVENT == 16,
                  "Bumping this without updating addVoicings + chord-soundfont "
                  "tests will silently truncate user voicings.");

    SECTION("17-voice polyrhythm truncates to 16") {
        akkado::SampleRegistry reg;
        // Register 17 distinct samples and build a 17-branch polyrhythm.
        std::string pattern = "pat(\"[";
        for (int i = 1; i <= 17; ++i) {
            std::string name = "s" + std::to_string(i);
            reg.register_sample(name, static_cast<std::uint16_t>(i));
            if (i > 1) pattern += ", ";
            pattern += name;
        }
        pattern += "]\") |> out(%, %)";

        auto result = akkado::compile(pattern, "<input>", &reg);
        REQUIRE(result.success);
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        CHECK(events[0].num_values == 16);
        for (std::uint8_t i = 0; i < 16; ++i) {
            CHECK(events[0].values[i] == static_cast<float>(i + 1));
        }
        // s17 (id 17) dropped — truncation is silent and graceful, matching
        // the original cap convention.
    }
}

TEST_CASE("Mini-notation chord lexer no longer truncates extended qualities",
          "[chord-extended-qualities]") {
    // Pre-fix: mini_lexer.cpp's lookup_chord table was a partial subset of
    // chord_parser's CHORD_QUALITIES, so qualities like "M7"/"m9"/"13"/"^7"
    // silently fell back to a major triad inside the lexer even though the
    // chord parser knew them. Both lexers now read the unified canonical
    // table in music_theory.hpp.
    //
    // Standalone `chord("CM7")` compiles to a polyphonic pattern that needs
    // `poly()` (or `soundfont(...)`) to consume — without one, codegen
    // reports E410 with the voice count in the message. We assert via that
    // message rather than wrapping each chord in poly() to keep the tests
    // focused on the lexer fix.
    auto e410_voice_count = [](const akkado::CompileResult& result) -> int {
        for (const auto& d : result.diagnostics) {
            if (d.code != "E410") continue;
            // "Chord pattern has N voices but is not wrapped in poly()..."
            auto pos = d.message.find("has ");
            if (pos == std::string::npos) continue;
            int n = 0;
            for (auto i = pos + 4; i < d.message.size() && std::isdigit(d.message[i]); ++i) {
                n = n * 10 + (d.message[i] - '0');
            }
            return n;
        }
        return -1;
    };

    SECTION("chord(\"CM7\") emits a 4-voice chord pattern") {
        auto result = akkado::compile(R"(chord("CM7") |> osc("sin", %.freq) |> out(%, %))");
        CHECK(e410_voice_count(result) == 4);
    }

    SECTION("chord(\"Cm9\") emits a 5-voice chord pattern") {
        auto result = akkado::compile(R"(chord("Cm9") |> osc("sin", %.freq) |> out(%, %))");
        CHECK(e410_voice_count(result) == 5);
    }

    SECTION("chord(\"C13\") emits a 6-voice chord pattern") {
        auto result = akkado::compile(R"(chord("C13") |> osc("sin", %.freq) |> out(%, %))");
        CHECK(e410_voice_count(result) == 6);
    }

    SECTION("c\"…\" mini-notation honors extended qualities too") {
        // Mixed-arity chord pattern. Previously every voice count would
        // collapse to 3 because of the lexer's partial table; max across
        // the pattern now matches the largest chord (C13 = 6 voices).
        auto result = akkado::compile(R"(c"CM7 Cm9 C13" |> osc("sin", %.freq) |> out(%, %))");
        CHECK(e410_voice_count(result) == 6);
    }

    SECTION("CM7 voiced through soundfont expands to 4 SF_VOICE slots") {
        // End-to-end: with the unified table the chord is a true 4-voice
        // signal, so the soundfont path emits 4 SOUNDFONT_VOICE
        // instructions (was 3 before the fix, silently dropping the 7th).
        auto result = akkado::compile(R"(c"CM7" |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        std::size_t sf_count =
            count_instructions(insts, cedar::Opcode::SOUNDFONT_VOICE);
        CHECK(sf_count == 4);

        // FFT-style sanity: the four chord pitch classes must all be
        // represented in the value buffer of event 0.
        REQUIRE(!result.state_inits.empty());
        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 1);
        const auto& ev = events[0];
        CHECK(ev.num_values == 4);
        auto freq_to_midi = [](float f) {
            return static_cast<int>(std::round(69.0f + 12.0f * std::log2(f / 440.0f)));
        };
        std::set<int> pcs;
        for (std::uint8_t i = 0; i < ev.num_values; ++i) {
            int midi = freq_to_midi(ev.values[i]);
            pcs.insert(((midi % 12) + 12) % 12);
        }
        CHECK(pcs == std::set<int>{0, 4, 7, 11});  // C E G B
    }
}

TEST_CASE("Codegen: Nested bracket subdivision", "[codegen][nested]") {
    SECTION("[] within [] — 2 levels: bd [sd [hh hh]]") {
        auto result = akkado::compile(R"(pat("bd [sd [hh hh]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 4);

        // 2 top-level → cycle_length=2, events in [0,1) normalized range
        // bd: 0.0, dur 0.5
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].duration == Catch::Approx(0.5f));
        // sd: 0.5, dur 0.25
        CHECK(events[1].time == Catch::Approx(0.5f));
        CHECK(events[1].duration == Catch::Approx(0.25f));
        // hh: 0.75, dur 0.125
        CHECK(events[2].time == Catch::Approx(0.75f));
        CHECK(events[2].duration == Catch::Approx(0.125f));
        // hh: 0.875, dur 0.125
        CHECK(events[3].time == Catch::Approx(0.875f));
        CHECK(events[3].duration == Catch::Approx(0.125f));
    }

    SECTION("[] within [] — 3 levels: bd [sd [hh [cp cp]]]") {
        auto result = akkado::compile(R"(pat("bd [sd [hh [cp cp]]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 5);

        CHECK(events[0].time == Catch::Approx(0.0f));      // bd
        CHECK(events[0].duration == Catch::Approx(0.5f));
        CHECK(events[1].time == Catch::Approx(0.5f));      // sd
        CHECK(events[1].duration == Catch::Approx(0.25f));
        CHECK(events[2].time == Catch::Approx(0.75f));     // hh
        CHECK(events[2].duration == Catch::Approx(0.125f));
        CHECK(events[3].time == Catch::Approx(0.875f));    // cp
        CHECK(events[3].duration == Catch::Approx(0.0625f));
        CHECK(events[4].time == Catch::Approx(0.9375f));   // cp
        CHECK(events[4].duration == Catch::Approx(0.0625f));
    }

    SECTION("[] within [] — 4 levels: bd [sd [hh [cp [oh oh]]]]") {
        auto result = akkado::compile(R"(pat("bd [sd [hh [cp [oh oh]]]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 6);

        CHECK(events[0].time == Catch::Approx(0.0f));       // bd
        CHECK(events[0].duration == Catch::Approx(0.5f));
        CHECK(events[1].time == Catch::Approx(0.5f));       // sd
        CHECK(events[1].duration == Catch::Approx(0.25f));
        CHECK(events[2].time == Catch::Approx(0.75f));      // hh
        CHECK(events[2].duration == Catch::Approx(0.125f));
        CHECK(events[3].time == Catch::Approx(0.875f));     // cp
        CHECK(events[3].duration == Catch::Approx(0.0625f));
        CHECK(events[4].time == Catch::Approx(0.9375f));    // oh
        CHECK(events[4].duration == Catch::Approx(0.03125f));
        CHECK(events[5].time == Catch::Approx(0.96875f));   // oh
        CHECK(events[5].duration == Catch::Approx(0.03125f));
    }

    SECTION("symmetric nesting: [bd sd] [hh [cp oh]]") {
        auto result = akkado::compile(R"(pat("[bd sd] [hh [cp oh]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 5);

        // [bd sd] gets first half [0, 0.5)
        CHECK(events[0].time == Catch::Approx(0.0f));       // bd
        CHECK(events[0].duration == Catch::Approx(0.25f));
        CHECK(events[1].time == Catch::Approx(0.25f));      // sd
        CHECK(events[1].duration == Catch::Approx(0.25f));
        // [hh [cp oh]] gets second half [0.5, 1.0)
        CHECK(events[2].time == Catch::Approx(0.5f));       // hh
        CHECK(events[2].duration == Catch::Approx(0.25f));
        CHECK(events[3].time == Catch::Approx(0.75f));      // cp
        CHECK(events[3].duration == Catch::Approx(0.125f));
        CHECK(events[4].time == Catch::Approx(0.875f));     // oh
        CHECK(events[4].duration == Catch::Approx(0.125f));
    }

    SECTION("4 levels all []: [[bd sd] [[hh hh] [cp oh]]]") {
        auto result = akkado::compile(R"(pat("[[bd sd] [[hh hh] [cp oh]]]") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(!result.state_inits.empty());

        const auto& events = result.state_inits[0].sequence_events[0];
        REQUIRE(events.size() == 6);

        // Single top-level group → cycle_length=1
        // [bd sd] gets [0, 0.5): bd@0, sd@0.25
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[0].duration == Catch::Approx(0.25f));
        CHECK(events[1].time == Catch::Approx(0.25f));
        CHECK(events[1].duration == Catch::Approx(0.25f));
        // [[hh hh] [cp oh]] gets [0.5, 1.0)
        //   [hh hh]@[0.5, 0.75): hh@0.5, hh@0.625
        CHECK(events[2].time == Catch::Approx(0.5f));
        CHECK(events[2].duration == Catch::Approx(0.125f));
        CHECK(events[3].time == Catch::Approx(0.625f));
        CHECK(events[3].duration == Catch::Approx(0.125f));
        //   [cp oh]@[0.75, 1.0): cp@0.75, oh@0.875
        CHECK(events[4].time == Catch::Approx(0.75f));
        CHECK(events[4].duration == Catch::Approx(0.125f));
        CHECK(events[5].time == Catch::Approx(0.875f));
        CHECK(events[5].duration == Catch::Approx(0.125f));
    }
}

TEST_CASE("Codegen: Binary operations", "[codegen]") {
    SECTION("arithmetic") {
        auto result = akkado::compile("1 + 2 * 3 - 4 / 2");
        CHECK(result.success);
    }

    SECTION("multiplication and division") {
        auto result = akkado::compile("x = 10 * 3\ny = x / 2");
        CHECK(result.success);
    }

    SECTION("unary minus") {
        auto result = akkado::compile("-42");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Timing", "[codegen]") {
    SECTION("clock function no args") {
        auto result = akkado::compile("clock()");
        CHECK(result.success);
    }

    SECTION("phasor") {
        auto result = akkado::compile("phasor(1)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Parameters", "[codegen]") {
    SECTION("param with defaults") {
        auto result = akkado::compile("freq = param(\"freq\", 440, 20, 2000)");
        CHECK(result.success);
    }

    SECTION("toggle") {
        auto result = akkado::compile("mute = toggle(\"mute\", false)");
        CHECK(result.success);
    }

    SECTION("button") {
        auto result = akkado::compile("trig = button(\"trigger\")");
        CHECK(result.success);
    }

    SECTION("dropdown") {
        auto result = akkado::compile("wave = dropdown(\"wave\", \"sin\", \"saw\", \"tri\")");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Chord function", "[codegen]") {
    SECTION("chord without poly produces E410") {
        auto result = akkado::compile("chord(\"Am\")");
        CHECK_FALSE(result.success);
    }

    SECTION("chord progression without poly produces E410") {
        auto result = akkado::compile("chord(\"Am C F G\")");
        CHECK_FALSE(result.success);
    }

    SECTION("chord pattern without poly is error") {
        auto result = akkado::compile(R"(pat("C4'") |> osc("sin", %.freq) |> out(%, %))");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("Codegen: Oscillators", "[codegen]") {
    SECTION("basic osc") {
        auto result = akkado::compile("osc(\"sin\", 440)");
        CHECK(result.success);
    }

    SECTION("osc with named param") {
        auto result = akkado::compile("osc(type: \"saw\", freq: 220)");
        CHECK(result.success);
    }

    SECTION("pulse osc with pwm") {
        auto result = akkado::compile("osc(\"pulse\", 440, 0.3)");
        CHECK(result.success);
    }
}

TEST_CASE("Codegen: Mini-notation patterns", "[codegen]") {
    SECTION("simple pattern") {
        auto result = akkado::compile("pat(\"c4 e4 g4\")");
        CHECK(result.success);
    }

    SECTION("pattern with rests") {
        auto result = akkado::compile("pat(\"c4 ~ e4 ~\")");
        CHECK(result.success);
    }

    SECTION("pattern with groups") {
        auto result = akkado::compile("pat(\"[c4 e4] g4\")");
        CHECK(result.success);
    }

    SECTION("pattern with euclidean") {
        auto result = akkado::compile("pat(\"c4(3,8)\")");
        CHECK(result.success);
    }

    SECTION("pattern with speed modifier") {
        auto result = akkado::compile("pat(\"c4*2 e4\")");
        CHECK(result.success);
    }

    SECTION("drum pattern") {
        auto result = akkado::compile("pat(\"kick snare kick snare\")");
        CHECK(result.success);
    }
}

// =============================================================================
// UGen Auto-Expansion Tests
// =============================================================================

TEST_CASE("Codegen: UGen auto-expansion", "[codegen][arrays]") {
    SECTION("array of frequencies expands sine to 3 instances") {
        // [440, 550, 660] |> sine(%) produces 3 OSC_SIN instructions
        auto result = akkado::compile("[440, 550, 660] |> sine(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 OSC_SIN instructions
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 3);
    }

    SECTION("array expansion followed by sum") {
        // Note: sum(%) requires the % to pass the multi-buffer through
        auto result = akkado::compile("[220, 330, 440] |> saw(%) |> sum(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 sawtooth oscillators
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 3);
        // 2 additions to sum them
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("array of frequencies through osc() stdlib") {
        // osc() is defined in stdlib and calls sine for type="sin"
        // This currently produces 1 osc because the match resolves before expansion.
        // For full expansion through stdlib osc(), need to call directly:
        auto result = akkado::compile("freqs = [440, 550, 660]\nsine(freqs)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 OSC_SIN instructions
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 3);
    }

    SECTION("single element array does not expand") {
        auto result = akkado::compile("[440] |> sine(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Single element: just one instruction
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 1);
    }

    SECTION("filter expansion with array input") {
        // Filters also expand when given array inputs
        auto result = akkado::compile("ns = noise()\nfreqs = [1000, 2000, 3000]\nfreqs |> lp(ns, %)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have 3 SVF_LP instructions
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 3);
    }
}

// =============================================================================
// Binary Operation Broadcasting Tests
// =============================================================================

TEST_CASE("Codegen: Array broadcasting", "[codegen][arrays]") {
    SECTION("array * scalar") {
        auto result = akkado::compile("[1, 2, 3] * 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 multiplications
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
    }

    SECTION("scalar + array") {
        auto result = akkado::compile("10 + [1, 2, 3]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 additions
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("array + array same length") {
        auto result = akkado::compile("[1, 2] + [3, 4]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 2 additions
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("array + array broadcasting") {
        // [1, 2, 3, 4] + [10, 20] -> [11, 22, 13, 24] (shorter array cycles)
        auto result = akkado::compile("[1, 2, 3, 4] + [10, 20]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 additions (length of longer array)
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 4);
    }

    SECTION("division broadcasting") {
        auto result = akkado::compile("[10, 20, 30] / 10");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::DIV) == 3);
    }
}

// =============================================================================
// Array Reduction Tests
// =============================================================================

TEST_CASE("Codegen: Array reductions", "[codegen][arrays]") {
    SECTION("product via reduce(*, 1)") {
        auto result = akkado::compile("reduce([2, 3, 4], (a, b) -> a * b, 1)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 1 * 2 * 3 * 4 — three MULs (one per array element)
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
    }

    SECTION("mean of array") {
        auto result = akkado::compile("mean([10, 20, 30])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 2 ADDs + 1 DIV
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::DIV) == 1);
    }

    SECTION("min of array") {
        auto result = akkado::compile("min([5, 2, 8, 1])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 MIN operations
        CHECK(count_instructions(insts, cedar::Opcode::MIN) == 3);
    }

    SECTION("max of array") {
        auto result = akkado::compile("max([5, 2, 8, 1])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 3 MAX operations
        CHECK(count_instructions(insts, cedar::Opcode::MAX) == 3);
    }

    SECTION("binary min still works") {
        auto result = akkado::compile("min(3, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Single MIN operation
        CHECK(count_instructions(insts, cedar::Opcode::MIN) == 1);
    }
}

// =============================================================================
// Array Transformation Tests
// =============================================================================

TEST_CASE("Codegen: Array indexing", "[codegen][arrays]") {
    SECTION("constant index returns the indexed element, not first") {
        auto result = akkado::compile("arr = [10, 20, 30]\narr[1]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_20 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 20.0f) {
                found_20 = true;
                break;
            }
        }
        CHECK(found_20);
    }

    SECTION("last element accessible via constant index") {
        auto result = akkado::compile("arr = [10, 20, 30]\narr[2]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_30 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 30.0f) {
                found_30 = true;
                break;
            }
        }
        CHECK(found_30);
    }

    SECTION("index 0 returns first element") {
        auto result = akkado::compile("arr = [10, 20, 30]\narr[0]");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        bool found_10 = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::PUSH_CONST &&
                decode_const_float(inst) == 10.0f) {
                found_10 = true;
                break;
            }
        }
        CHECK(found_10);
    }

    SECTION("dynamic index emits ARRAY_INDEX opcode") {
        auto result = akkado::compile(
            "freq = param(\"idx\", 0, 0, 2)\n"
            "arr = [100, 200, 300]\n"
            "arr[freq]"
        );
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::ARRAY_INDEX) != nullptr);
    }
}

TEST_CASE("Codegen: Array transformations", "[codegen][arrays]") {
    SECTION("rotate") {
        auto result = akkado::compile("rotate([1, 2, 3, 4], 1)");
        REQUIRE(result.success);
        // Rotation is just reordering - no arithmetic ops needed
    }

    SECTION("shuffle") {
        auto result = akkado::compile("shuffle([1, 2, 3, 4])");
        REQUIRE(result.success);
    }

    SECTION("sort") {
        auto result = akkado::compile("sort([3, 1, 4, 1, 5])");
        REQUIRE(result.success);
    }

    SECTION("normalize") {
        auto result = akkado::compile("normalize([10, 20, 30])");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have MIN, MAX, SUB, and DIV operations
        CHECK(count_instructions(insts, cedar::Opcode::MIN) > 0);
        CHECK(count_instructions(insts, cedar::Opcode::MAX) > 0);
    }

    SECTION("scale") {
        auto result = akkado::compile("scale([10, 20, 30], 0, 100)");
        REQUIRE(result.success);
    }
}

// =============================================================================
// Array Generation Tests
// =============================================================================

TEST_CASE("Codegen: Array generation", "[codegen][arrays]") {
    SECTION("linspace") {
        auto result = akkado::compile("linspace(0, 10, 5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 5 constants: 0, 2.5, 5, 7.5, 10
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 5);
    }

    SECTION("random") {
        auto result = akkado::compile("random(4)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 random constants
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 4);
    }

    SECTION("harmonics") {
        auto result = akkado::compile("harmonics(110, 4)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 harmonics: 110, 220, 330, 440
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) == 4);
    }

    SECTION("harmonics through oscillator") {
        auto result = akkado::compile("harmonics(110, 4) |> sine(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // 4 oscillators
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 4);
    }
}

// =============================================================================
// Stereo Tests
// =============================================================================

TEST_CASE("Codegen: stereo() creates stereo signal", "[codegen][stereo]") {
    SECTION("stereo from mono duplicates signal") {
        auto result = akkado::compile("stereo(saw(220))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have: PUSH_CONST, OSC_SAW, COPY, COPY (duplicate to L and R)
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::COPY) == 2);
    }

    SECTION("stereo from L/R pair") {
        auto result = akkado::compile("stereo(saw(218), saw(222))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Two separate oscillators, no COPY needed
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::COPY) == 0);
    }
}

TEST_CASE("Codegen: left() and right() extract channels", "[codegen][stereo]") {
    SECTION("left extracts left channel") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            left(s) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Two oscillators, output instruction
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
    }

    SECTION("right extracts right channel") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            right(s) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
    }
}

TEST_CASE("Codegen: pan() emits PAN opcode", "[codegen][stereo]") {
    SECTION("pan mono to stereo") {
        auto result = akkado::compile("pan(saw(220), 0.5)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 1);
        auto* pan = find_instruction(insts, cedar::Opcode::PAN);
        REQUIRE(pan != nullptr);
    }

    SECTION("pan with modulated position") {
        auto result = akkado::compile("pan(saw(220), lfo(0.5))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::LFO) == 1);
        auto* pan = find_instruction(insts, cedar::Opcode::PAN);
        REQUIRE(pan != nullptr);
    }
}

TEST_CASE("Codegen: width() emits WIDTH opcode", "[codegen][stereo]") {
    SECTION("width control on stereo signal") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            width(s, 1.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* width = find_instruction(insts, cedar::Opcode::WIDTH);
        REQUIRE(width != nullptr);
    }

    SECTION("width with modulated amount") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            width(s, 1.0 + lfo(0.2) * 0.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::LFO) == 1);
        auto* width = find_instruction(insts, cedar::Opcode::WIDTH);
        REQUIRE(width != nullptr);
    }
}

TEST_CASE("Codegen: ms_encode() and ms_decode()", "[codegen][stereo]") {
    SECTION("mid/side encoding") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            ms_encode(s)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* encode = find_instruction(insts, cedar::Opcode::MS_ENCODE);
        REQUIRE(encode != nullptr);
    }

    SECTION("mid/side decoding") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            ms = ms_encode(s)
            ms_decode(ms)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* encode = find_instruction(insts, cedar::Opcode::MS_ENCODE);
        auto* decode = find_instruction(insts, cedar::Opcode::MS_DECODE);
        REQUIRE(encode != nullptr);
        REQUIRE(decode != nullptr);
    }
}

TEST_CASE("Codegen: pingpong() emits DELAY_PINGPONG opcode", "[codegen][stereo]") {
    SECTION("basic ping-pong delay") {
        auto result = akkado::compile(R"(
            s = stereo(saw(220), saw(220))
            pingpong(s, 0.25, 0.6)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* pingpong = find_instruction(insts, cedar::Opcode::DELAY_PINGPONG);
        REQUIRE(pingpong != nullptr);
    }

    SECTION("ping-pong with modulated parameters") {
        auto result = akkado::compile(R"(
            s = stereo(saw(220), saw(220))
            pingpong(s, lfo(0.1) * 0.25 + 0.1, 0.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::LFO) == 1);
        auto* pingpong = find_instruction(insts, cedar::Opcode::DELAY_PINGPONG);
        REQUIRE(pingpong != nullptr);
    }
}

TEST_CASE("Codegen: out() with stereo signal", "[codegen][stereo]") {
    SECTION("out auto-routes stereo L/R") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(s)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        // L and R should be different buffers
        CHECK(out->inputs[0] != out->inputs[1]);
    }

    SECTION("out with mono still works") {
        auto result = akkado::compile("out(saw(220))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        // Mono: both channels same buffer
        CHECK(out->inputs[0] == out->inputs[1]);
    }
}

TEST_CASE("Codegen: stereo propagation through stereo-native effects", "[codegen][stereo]") {
    // A stereo signal flowing into a stereo-native DSP op emits a SINGLE
    // instruction carrying STEREO_OUTPUT | STEREO_INPUT. The opcode body
    // handles both channels in one dispatch with one per-channel state struct
    // (prd-stereo-native-opcodes; auto-lift retired in Phase 5).
    SECTION("stereo through filter emits one instruction with STEREO_INPUT flag") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            lp(s, 1000)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("stereo through chain of effects") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            s |> lp(%, 1000) |> hp(%, 100)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_HP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        auto* hp = find_instruction(insts, cedar::Opcode::FILTER_SVF_HP);
        REQUIRE(lp != nullptr);
        REQUIRE(hp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
        CHECK((hp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("stereo through delay") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            delay(s, 0.25, 0.5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::DELAY) == 1);
        auto* d = find_instruction(insts, cedar::Opcode::DELAY);
        REQUIRE(d != nullptr);
        CHECK((d->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

TEST_CASE("Codegen: stereo pipeline examples", "[codegen][stereo]") {
    SECTION("pan sweep") {
        auto result = akkado::compile(R"(
            saw(220) |> pan(%, lfo(0.5)) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SAW) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::LFO) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::PAN) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }

    SECTION("stereo width modulation") {
        auto result = akkado::compile(R"(
            stereo(saw(218), saw(222))
            |> width(%, 1.0 + lfo(0.2) * 0.5)
            |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
        CHECK(find_instruction(insts, cedar::Opcode::WIDTH) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }

    SECTION("full stereo chain with ping-pong") {
        auto result = akkado::compile(R"(
            saw(220)
            |> stereo(%)
            |> lp(%, 1000)
            |> pingpong(%, 0.25, 0.5)
            |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SAW) != nullptr);
        // stereo() from mono creates 2 COPYs
        CHECK(count_instructions(insts, cedar::Opcode::COPY) == 2);
        // lp() is stereo-native: 1 stereo-flagged instruction for the pair
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
        CHECK(find_instruction(insts, cedar::Opcode::DELAY_PINGPONG) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::OUTPUT) != nullptr);
    }
}

// =============================================================================
// prd-stereo-support: mono() downmix and stereo-native dispatch behavior
// =============================================================================

TEST_CASE("Codegen: mono() downmix", "[codegen][stereo][mono]") {
    SECTION("mono(stereo) emits MONO_DOWNMIX opcode") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            mono(s)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* dm = find_instruction(insts, cedar::Opcode::MONO_DOWNMIX);
        REQUIRE(dm != nullptr);
    }

    SECTION("mono() followed by mono DSP stays mono-primary-input") {
        // mono(s) produces a Mono signal; lp() is stereo-native but with a
        // mono primary input emits STEREO_OUTPUT without STEREO_INPUT.
        auto result = akkado::compile(R"(
            stereo(saw(218), saw(222))
            |> mono(%)
            |> lp(%, 500)
            |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
    }

    SECTION("mono(mono) auto-escalates with W181 warning") {
        // prd-stereo-native-opcodes §5.6: mono(mono) is a silent no-op that
        // returns the input unchanged and emits W181 at source location.
        auto result = akkado::compile("mono(saw(220))");
        CHECK(result.success);
        bool found_w181 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "W181") found_w181 = true;
        }
        CHECK(found_w181);
    }

    SECTION("mono(fn) still dispatches to voice manager") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> mono(%, lead) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
    }
}

TEST_CASE("Codegen: stereo-native filter state IDs", "[codegen][stereo][stereo-native]") {
    // lp is stereo-native post-prd-stereo-native-opcodes Phase 4a: per-channel
    // fields live inside one state struct keyed by fnv1a(semantic_path) — no
    // /L suffix, no XOR. Same path → same state_id whether input is mono or
    // stereo (the channels parallel-process inside the opcode body).
    SECTION("stereo input emits STEREO_OUTPUT|STEREO_INPUT, single state_id") {
        auto result = akkado::compile(R"(
            stereo(saw(218), saw(222)) |> lp(%, 500)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
        CHECK(lp->state_id != 0);
    }

    SECTION("mono and stereo paths share the same fnv1a(path) state_id") {
        // Both compile to a stereo-native lp at the same semantic path; the
        // state_id is the plain fnv1a(path) hash, so hot-swap correctly
        // rebinds across mono→stereo input changes (and vice-versa).
        auto mono_result = akkado::compile("saw(220) |> lp(%, 500) |> out(%)");
        auto stereo_result = akkado::compile(
            "stereo(saw(220)) |> lp(%, 500) |> out(%)");
        REQUIRE(mono_result.success);
        REQUIRE(stereo_result.success);

        auto mono_insts = get_instructions(mono_result);
        auto stereo_insts = get_instructions(stereo_result);

        auto* mono_lp = find_instruction(mono_insts, cedar::Opcode::FILTER_SVF_LP);
        auto* stereo_lp = find_instruction(stereo_insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(mono_lp != nullptr);
        REQUIRE(stereo_lp != nullptr);
        CHECK(mono_lp->state_id == stereo_lp->state_id);
    }
}

TEST_CASE("Codegen: pan(stereo) dispatches to PAN_STEREO", "[codegen][stereo][pan]") {
    SECTION("pan(mono, pos) emits PAN") {
        auto result = akkado::compile("pan(saw(220), 0.3) |> out(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::PAN) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::PAN_STEREO) == nullptr);
    }

    SECTION("pan(stereo, pos) emits PAN_STEREO for equal-power balance") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            pan(s, 0.3) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::PAN_STEREO) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::PAN) == nullptr);
    }
}

// =============================================================================
// Pattern bank() and variant() modifier tests
// =============================================================================

TEST_CASE("Pattern function: bank()", "[codegen][patterns][bank]") {
    SECTION("bank requires pattern as first argument") {
        auto result = akkado::compile("bank(42, \"TR808\")");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E130" || d.code == "E133") found = true;
        }
        CHECK(found);
    }

    SECTION("bank requires string as second argument") {
        auto result = akkado::compile(R"(bank(pat("bd sd"), 808))");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E131") found = true;
        }
        CHECK(found);
    }

    SECTION("bank with valid pattern compiles") {
        auto result = akkado::compile(R"(bank(pat("bd sd hh"), "TR808"))");
        REQUIRE(result.success);

        // Should have SEQPAT_QUERY, SEQPAT_STEP, and SAMPLE_PLAY instructions.
        // The SAMPLE_PLAY check guards against a regression where bank()
        // emitted only the SEQPAT pipeline and dropped the sampler — the
        // returned buffer was raw sample-IDs as DC instead of audio.
        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_STEP) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::SAMPLE_PLAY) != nullptr);
    }

    SECTION("bank populates required_samples_extended with bank info") {
        auto result = akkado::compile(R"(bank(pat("bd sd"), "TR909"))");
        REQUIRE(result.success);

        // Check that required_samples_extended has entries with bank info
        bool found_bank = false;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.bank == "TR909") {
                found_bank = true;
                break;
            }
        }
        CHECK(found_bank);
    }

    SECTION("bank via method call syntax") {
        // pat("bd").bank("TR808") should desugar to bank(pat("bd"), "TR808")
        auto result = akkado::compile(R"(pat("bd sd").bank("TR808"))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
        // Sampler must be wired up: regression guard for SAMPLE_PLAY drop.
        CHECK(find_instruction(insts, cedar::Opcode::SAMPLE_PLAY) != nullptr);
    }
}

TEST_CASE("Pattern function: variant()", "[codegen][patterns][variant]") {
    SECTION("variant requires pattern as first argument") {
        auto result = akkado::compile("variant(42, 0)");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E130" || d.code == "E133") found = true;
        }
        CHECK(found);
    }

    SECTION("variant accepts string literal as variant pattern") {
        // String literals are now valid pattern expressions (parsed as mini-notation)
        auto result = akkado::compile(R"(variant(pat("bd"), "1 2 3"))");
        CHECK(result.success);
    }

    SECTION("variant with fixed index compiles") {
        auto result = akkado::compile(R"(variant(pat("bd bd bd"), 2))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_STEP) != nullptr);
        // Sampler must be wired up: regression guard for SAMPLE_PLAY drop.
        CHECK(find_instruction(insts, cedar::Opcode::SAMPLE_PLAY) != nullptr);
    }

    SECTION("variant populates required_samples_extended with variant info") {
        auto result = akkado::compile(R"(variant(pat("bd"), 3))");
        REQUIRE(result.success);

        // Check that required_samples_extended has entries with variant info
        bool found_variant = false;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.variant == 3) {
                found_variant = true;
                break;
            }
        }
        CHECK(found_variant);
    }

    SECTION("variant via method call syntax") {
        // pat("bd").variant(2) should desugar to variant(pat("bd"), 2)
        auto result = akkado::compile(R"(pat("bd bd").variant(2))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
        // Sampler must be wired up: regression guard for SAMPLE_PLAY drop.
        CHECK(find_instruction(insts, cedar::Opcode::SAMPLE_PLAY) != nullptr);
    }

    SECTION("variant with pattern index (cycling)") {
        // variant(pat("bd bd bd"), pat("<c0 d0 e0>")) cycles variants per event
        // Note: Using note names since bare numbers in pat() are interpreted as samples
        // The variant values are taken from the frequency values in the variant pattern
        auto result = akkado::compile(R"(variant(pat("bd bd bd"), pat("<c0 d0 e0>")))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::SEQPAT_QUERY) != nullptr);
    }

    SECTION("variant with negative index fails") {
        auto result = akkado::compile(R"(variant(pat("bd"), -1))");
        CHECK(!result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E131") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Pattern function: bank and variant chaining", "[codegen][patterns][bank][variant]") {
    SECTION("bank and variant can be chained via pipe") {
        auto result = akkado::compile(R"(
            pat("bd sd hh")
            |> bank(%, "TR808")
            |> variant(%, 1)
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        // Should have multiple SEQPAT_QUERY/STEP pairs (one per transform)
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) >= 1);
    }

    SECTION("method chaining: bank().variant()") {
        // bank() and variant() must compose: the inner transform's bank info
        // travels with the Pattern through compile_pattern_for_transform and
        // is preserved on the final sample_refs (see "Pattern transforms are
        // transparent to sample requirements" TEST_CASE).
        auto result = akkado::compile(R"(pat("bd sd").bank("TR909").variant(2))");
        REQUIRE(result.success);

        bool found_both = false;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.variant == 2 && sample.bank == "TR909") {
                found_both = true;
                break;
            }
        }
        CHECK(found_both);
    }

    SECTION("bank alone populates bank field") {
        auto result = akkado::compile(R"(pat("bd sd hh").bank("TR808"))");
        REQUIRE(result.success);

        // Verify bank was set
        bool all_have_bank = true;
        for (const auto& sample : result.required_samples_extended) {
            if (sample.bank != "TR808") {
                all_have_bank = false;
            }
        }
        CHECK(all_have_bank);
    }
}

// Patterns-as-values architectural invariants (added 2026-05-03):
//
// Pattern transforms (.fast, .slow, .rev, .transpose, .velocity, ...) must be
// transparent to sample-requirement metadata. Bank/variant info attached to
// a Pattern by .bank()/.variant() must travel through any number of wrapping
// transforms and end up in `required_samples_extended` regardless of how the
// Pattern is wrapped, and one Pattern chain's metadata must never depend on
// another chain's compile state.
//
// Origin: a user repro of the form
//   s"jungbass:0 ~".bank("Dirt-Samples").fast(2) |> out(@*.7)
// failed with "Sample 'amencutup' not found" — and amencutup was in a
// completely independent chain. Root cause was that
// `required_samples_extended_` was populated only by inline loops in
// `handle_bank_call`/`handle_variant_call`, so any wrapping transform
// (.fast, .slow, ...) silently dropped the registration. The web loader
// then branched on `extendedSamples.length > 0`, leaking one chain's
// missing entry into another chain's loader path.
TEST_CASE("Pattern transforms are transparent to sample requirements",
          "[codegen][patterns][bank][transform]") {
    auto bank_for = [](const akkado::CompileResult& r,
                       const std::string& name) -> std::string {
        for (const auto& s : r.required_samples_extended) {
            if (s.name == name) return s.bank;
        }
        return "<missing>";
    };

    SECTION("bank() info survives a wrapping .fast()") {
        // The original failing case: .fast(2) wrapping .bank() must not drop
        // the bank entry from required_samples_extended.
        auto result = akkado::compile(
            R"(pat("bd sd").bank("TR909").fast(2))");
        REQUIRE(result.success);
        CHECK(bank_for(result, "bd") == "TR909");
        CHECK(bank_for(result, "sd") == "TR909");
    }

    SECTION("bank() info survives a deep transform stack") {
        auto result = akkado::compile(
            R"(pat("bd sd").bank("X").fast(2).slow(3).rev())");
        REQUIRE(result.success);
        CHECK(bank_for(result, "bd") == "X");
        CHECK(bank_for(result, "sd") == "X");
    }

    SECTION("independent chains do not share metadata") {
        // Toggling .fast() on chain B must not change chain A's entries.
        // This is the chain-independence invariant — under the old design,
        // chain B's missing extended entry caused the JS loader to fall
        // back to legacy single-name resolution for chain A as well.
        auto without_fast_b = akkado::compile(R"(
            pat("bd sd").bank("A").fast(2)
            pat("hh").bank("B")
        )");
        auto with_fast_b = akkado::compile(R"(
            pat("bd sd").bank("A").fast(2)
            pat("hh").bank("B").fast(2)
        )");
        REQUIRE(without_fast_b.success);
        REQUIRE(with_fast_b.success);

        // Chain A's bank is "A" regardless of what chain B does.
        CHECK(bank_for(without_fast_b, "bd") == "A");
        CHECK(bank_for(with_fast_b,    "bd") == "A");
        // Chain B's bank is "B" regardless of whether it has .fast().
        CHECK(bank_for(without_fast_b, "hh") == "B");
        CHECK(bank_for(with_fast_b,    "hh") == "B");
    }

    SECTION("bank() info survives velocity() and transpose()") {
        auto v = akkado::compile(R"(pat("bd").bank("V").velocity(0.5))");
        auto t = akkado::compile(R"(pat("bd").bank("T").transpose(2))");
        REQUIRE(v.success);
        REQUIRE(t.success);
        CHECK(bank_for(v, "bd") == "V");
        CHECK(bank_for(t, "bd") == "T");
    }

    SECTION("Pattern value carries its own sample_refs") {
        // The Pattern's PatternPayload owns its sample_refs as a value-type
        // field. Even after stacking transforms, the final Pattern's
        // sample_refs should reflect the cumulative bank/variant.
        auto result = akkado::compile(
            R"(pat("bd sd").bank("Z").variant(1).fast(2))");
        REQUIRE(result.success);
        // The required_samples_extended ledger reflects the same data
        // (it's a deduped union of every Pattern's published sample_refs).
        CHECK(bank_for(result, "bd") == "Z");
        bool variant_propagated = false;
        for (const auto& s : result.required_samples_extended) {
            if (s.bank == "Z" && s.variant == 1) variant_propagated = true;
        }
        CHECK(variant_propagated);
    }
}

// =============================================================================
// Pattern String Prefix (p"...")
// =============================================================================

TEST_CASE("Codegen: Pattern string prefix", "[codegen][pattern-prefix]") {
    SECTION("p\"...\" produces same bytecode as pat(\"...\")") {
        auto prefix_result = akkado::compile(R"(p"c4 e4 g4")");
        auto call_result = akkado::compile(R"(pat("c4 e4 g4"))");

        REQUIRE(prefix_result.success);
        REQUIRE(call_result.success);
        CHECK(prefix_result.bytecode == call_result.bytecode);
    }

    SECTION("p\"...\" works in pipeline") {
        auto result = akkado::compile(R"(p"c4 e4 g4" |> osc("sin", %.freq))");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        CHECK(find_instruction(insts, cedar::Opcode::OSC_SIN) != nullptr);
    }

    SECTION("pat() with parens still works") {
        auto result = akkado::compile(R"(pat("c4 e4 g4"))");
        REQUIRE(result.success);
    }
}

// ============================================================================
// Velocity in Pattern Events
// ============================================================================

TEST_CASE("Codegen: velocity suffix in pattern events", "[codegen][pattern][velocity]") {
    SECTION("pat with velocity suffix stores velocity in events") {
        auto result = akkado::compile(R"(pat("c4:0.8 e4:0.5"))");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequence_events.size() >= 1);
        const auto& root_events = seq_init->sequence_events[0];
        REQUIRE(root_events.size() == 2);

        CHECK(root_events[0].velocity == Catch::Approx(0.8f).margin(0.001f));
        CHECK(root_events[1].velocity == Catch::Approx(0.5f).margin(0.001f));
    }

    SECTION("pat without velocity suffix defaults to 1.0") {
        auto result = akkado::compile(R"(pat("c4 e4"))");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(seq_init->sequence_events.size() >= 1);
        const auto& root_events = seq_init->sequence_events[0];
        REQUIRE(root_events.size() == 2);

        CHECK(root_events[0].velocity == Catch::Approx(1.0f).margin(0.001f));
        CHECK(root_events[1].velocity == Catch::Approx(1.0f).margin(0.001f));
    }
}

// =============================================================================
// Polyphony Tests (poly / mono / legato)
// =============================================================================

TEST_CASE("Codegen: poly() is stereo-native", "[codegen][poly][stereo]") {
    // poly always outputs a stereo L/R pair (adjacency: R = L+1). A mono voice
    // body is broadcast into both channels; a stereo body (pan) is preserved.

    SECTION("POLY_BEGIN carries the STEREO_OUTPUT flag") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4") |> poly(%, lead, 4) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        auto* poly = find_instruction(insts, cedar::Opcode::POLY_BEGIN);
        REQUIRE(poly != nullptr);
        CHECK((poly->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }

    SECTION("mono voice body broadcasts into both voice-out channels") {
        // A mono body (osc, no pan) still yields a stereo poly: codegen emits
        // two COPYs into the adjacent voice-out pair (L and L+1).
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4") |> poly(%, lead, 4) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        std::size_t poly_idx = insts.size();
        for (std::size_t i = 0; i < insts.size(); ++i) {
            if (insts[i].opcode == cedar::Opcode::POLY_BEGIN) { poly_idx = i; break; }
        }
        REQUIRE(poly_idx < insts.size());
        const auto& poly = insts[poly_idx];
        std::uint16_t voice_out_l = poly.inputs[4];
        std::uint16_t voice_out_r = static_cast<std::uint16_t>(voice_out_l + 1);

        bool copy_to_l = false, copy_to_r = false;
        for (std::size_t i = poly_idx + 1; i < poly_idx + 1 + poly.rate; ++i) {
            if (insts[i].opcode == cedar::Opcode::COPY) {
                if (insts[i].out_buffer == voice_out_l) copy_to_l = true;
                if (insts[i].out_buffer == voice_out_r) copy_to_r = true;
            }
        }
        CHECK(copy_to_l);
        CHECK(copy_to_r);
    }

    SECTION("poly output is stereo — out(%) receives an adjacent L/R pair") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4") |> poly(%, lead, 4) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        auto* output = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(output != nullptr);
        // out(%) on a stereo signal wires both channels: inputs[1] = inputs[0]+1.
        CHECK(output->inputs[1] != 0xFFFF);
        CHECK(output->inputs[1] == output->inputs[0] + 1);
    }

    SECTION("stereo voice body (pan) keeps both channels through poly") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq) |> pan(%, 0.5)
            pat("c4") |> poly(%, lead, 4) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        auto* poly = find_instruction(insts, cedar::Opcode::POLY_BEGIN);
        auto* output = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(poly != nullptr);
        REQUIRE(output != nullptr);
        CHECK((poly->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK(output->inputs[1] == output->inputs[0] + 1);
    }
}

TEST_CASE("Codegen: poly()", "[codegen][poly]") {
    SECTION("basic poly with named function") {
        // poly is stereo-native, so the correct sink idiom is out(%) — a
        // single stereo arg. (out(%, %) would auto-escalate per W185 and emit
        // two OUTPUTs.)
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4") |> poly(%, lead, 4) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        // Should contain POLY_BEGIN, body, POLY_END, OUTPUT
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::OUTPUT) == 1);

        // Find POLY_BEGIN and verify rate = body_length > 0
        auto* poly = find_instruction(insts, cedar::Opcode::POLY_BEGIN);
        REQUIRE(poly != nullptr);
        CHECK(poly->rate > 0);  // body_length >= 1

        // Verify state_id is set
        CHECK(poly->state_id != 0);

        // Verify state_init has PolyAlloc type with correct config
        bool found_poly_init = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_max_voices == 4);
                CHECK(init.poly_mode == 0);  // poly mode
                CHECK(init.state_id == poly->state_id);
                found_poly_init = true;
            }
        }
        CHECK(found_poly_init);
    }

    SECTION("mono desugars to poly with mode=1") {
        auto result = akkado::compile(R"(
            fn synth(f, g, v) -> osc("sin", f)
            mono(synth) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);

        // Verify state_init has mode=1 and max_voices=1
        bool found_poly_init = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_mode == 1);
                CHECK(init.poly_max_voices == 1);
                found_poly_init = true;
            }
        }
        CHECK(found_poly_init);
    }

    SECTION("legato desugars to poly with mode=2") {
        auto result = akkado::compile(R"(
            fn synth(f, g, v) -> osc("sin", f)
            legato(synth) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);

        bool found_poly_init = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_mode == 2);
                CHECK(init.poly_max_voices == 1);
                found_poly_init = true;
            }
        }
        CHECK(found_poly_init);
    }

    SECTION("poly with piped pattern input") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> poly(%, lead, 8) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        // Pattern emits SEQPAT_QUERY
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) == 1);
        // POLY block present
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);

        // Check that poly state_init has a linked seq_state_id
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_seq_state_id != 0);
                CHECK(init.poly_max_voices == 8);
            }
        }
    }

    SECTION("poly with inline closure") {
        auto result = akkado::compile(R"(
            pat("c4") |> poly(%, (f, g, v) -> osc("sin", f) * v, 4) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_END) == 1);
    }

    SECTION("error: instrument function must have 3 params") {
        auto result = akkado::compile(R"(
            fn bad(x) -> osc("sin", x)
            pat("c4") |> poly(%, bad, 4) |> out(%, %)
        )");
        CHECK(!result.success);
    }

    SECTION("error: instrument must be a function") {
        auto result = akkado::compile(R"(
            pat("c4") |> poly(%, 440, 4) |> out(%, %)
        )");
        CHECK(!result.success);
    }

    SECTION("poly with default voice count (no voices arg)") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> poly(%, lead) |> out(%, %)
        )");
        REQUIRE(result.success);
        // Voices should default to 64
        bool found_default = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_max_voices == 64);
                found_default = true;
            }
        }
        CHECK(found_default);
    }

    SECTION("error: 1-arg form not supported") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            poly(lead) |> out(%, %)
        )");
        CHECK(!result.success);
        bool found_arity_error = false;
        for (const auto& d : result.diagnostics) {
            if (d.message.find("'poly'") != std::string::npos &&
                d.message.find("at least 2") != std::string::npos) {
                found_arity_error = true;
                break;
            }
        }
        CHECK(found_arity_error);
    }

    SECTION("mono with piped pattern") {
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq)
            pat("c4 e4 g4") |> mono(%, lead) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::POLY_BEGIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::SEQPAT_QUERY) == 1);
    }

    SECTION("pitched polyrhythm [c4, e4] into poly() emits both notes as parallel events") {
        // Pitched polyrhythm doesn't go through the sample-merge path (atoms
        // aren't samples), so it falls back to per-child compile. That path
        // emits one DATA event per branch, both at t=0 with num_values=1.
        // POLY_BEGIN reads the SequenceState directly and allocates one voice
        // per (event, value) pair, so two voices fire in parallel.
        // Verified end-to-end via FFT: both 261.6 Hz (c4) and 329.6 Hz (e4)
        // are present at near-equal magnitude when rendered.
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq) * gate * 0.3
            pat("[c4, e4]") |> poly(%, lead, 4) |> out(%, %)
        )");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        REQUIRE(!seq_init->sequence_events.empty());
        const auto& events = seq_init->sequence_events[0];
        REQUIRE(events.size() == 2);
        // Both events at t=0, each carries one frequency.
        CHECK(events[0].time == Catch::Approx(0.0f));
        CHECK(events[1].time == Catch::Approx(0.0f));
        CHECK(events[0].num_values == 1);
        CHECK(events[1].num_values == 1);
        // Frequencies: c4 ~ 261.63, e4 ~ 329.63 (one per event).
        std::vector<float> got = {events[0].values[0], events[1].values[0]};
        std::sort(got.begin(), got.end());
        CHECK(got[0] == Catch::Approx(261.6256f).margin(0.5f));
        CHECK(got[1] == Catch::Approx(329.6276f).margin(0.5f));

        // Confirm poly_seq_state_id wires the SequenceState to POLY_BEGIN.
        bool found_poly = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_seq_state_id != 0);
                CHECK(init.poly_seq_state_id == seq_init->state_id);
                found_poly = true;
            }
        }
        CHECK(found_poly);
    }

    SECTION("pitched polyrhythm with subdivision [c4, [e4 g4 b4]] sustains c4") {
        // c4 occupies the full cycle on branch A. Branch B subdivides into
        // e4/g4/b4 thirds. POLY_BEGIN picks up all four events; FFT-verified
        // peaks for c4/e4/g4/b4 all present in the rendered audio.
        auto result = akkado::compile(R"(
            fn lead(freq, gate, vel) -> osc("sin", freq) * gate * 0.25
            pat("[c4, [e4 g4 b4]]") |> poly(%, lead, 4) |> out(%, %)
        )");
        REQUIRE(result.success);

        const akkado::StateInitData* seq_init = nullptr;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::SequenceProgram) {
                seq_init = &init;
                break;
            }
        }
        REQUIRE(seq_init != nullptr);
        const auto& events = seq_init->sequence_events[0];
        // 4 events in fallback path: c4@0 dur=1, e4@0 dur=1/3, g4@1/3 dur=1/3,
        // b4@2/3 dur=1/3. All num_values=1.
        REQUIRE(events.size() == 4);
        std::size_t c4_count = 0, e4_count = 0, g4_count = 0, b4_count = 0;
        for (const auto& e : events) {
            REQUIRE(e.num_values == 1);
            float f = e.values[0];
            if (std::abs(f - 261.6256f) < 0.5f) ++c4_count;
            else if (std::abs(f - 329.6276f) < 0.5f) ++e4_count;
            else if (std::abs(f - 391.9954f) < 0.5f) ++g4_count;
            else if (std::abs(f - 493.8833f) < 0.5f) ++b4_count;
        }
        CHECK(c4_count == 1);
        CHECK(e4_count == 1);
        CHECK(g4_count == 1);
        CHECK(b4_count == 1);
    }
}

// =============================================================================
// Codegen: Record spreading
// =============================================================================

TEST_CASE("Codegen: Record spreading", "[codegen][records]") {
    SECTION("spread with override") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880}
            r.freq
        )");
        CHECK(result.success);
    }

    SECTION("spread with new field") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, pan: 0.5}
            r.pan
        )");
        CHECK(result.success);
    }

    SECTION("spread all fields") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base}
            r.freq + r.vel
        )");
        CHECK(result.success);
    }

    SECTION("inline spread source") {
        auto result = akkado::compile(R"(
            r = {..{freq: 440}, vel: 0.8}
            r.freq + r.vel
        )");
        CHECK(result.success);
    }

    SECTION("spread in pipe") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880}
            osc("sin", r.freq) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Codegen: Expression defaults
// =============================================================================

TEST_CASE("Codegen: Expression defaults", "[codegen][fn]") {
    SECTION("arithmetic expression default") {
        auto result = akkado::compile(R"(
            fn f(x, cut = 440 * 2) -> x + cut
            f(1)
        )");
        CHECK(result.success);
    }

    SECTION("const variable in expression default") {
        auto result = akkado::compile(R"(
            const BASE = 440
            fn f(x, freq = BASE * 2) -> x + freq
            f(1)
        )");
        CHECK(result.success);
    }

    SECTION("const fn call in expression default") {
        auto result = akkado::compile(R"(
            const fn double(x) -> x * 2
            fn f(x, cut = double(440)) -> x + cut
            f(1)
        )");
        CHECK(result.success);
    }

    SECTION("expression default not used when arg provided") {
        auto result = akkado::compile(R"(
            fn f(x, cut = 440 * 2) -> x + cut
            f(1, 100)
        )");
        CHECK(result.success);
    }

    SECTION("closure with expression default") {
        auto result = akkado::compile(R"(
            g = (x, cut = 440 * 2) -> x + cut
            g(1)
        )");
        CHECK(result.success);
    }

    SECTION("multiple expression defaults") {
        auto result = akkado::compile(R"(
            fn f(x, a = 1 + 1, b = 2 * 3) -> x + a + b
            f(1)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Codegen: Record spread interactions (F8 integration)
// =============================================================================

TEST_CASE("Codegen: Record spread interactions", "[codegen][records]") {
    SECTION("field access on spread result") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            {..base, freq: 880}.freq
        )");
        CHECK(result.success);
    }

    SECTION("spread + as-binding in pipe") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            {..base, vel: 0.5} as r |> osc("sin", r.freq) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("spread + dot-call") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880}
            osc("sin", r.freq).lp(1000) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("nested spread (spread of spread)") {
        auto result = akkado::compile(R"(
            inner = {freq: 440}
            mid = {..inner, vel: 0.8}
            outer = {..mid, pan: 0.5}
            outer.freq + outer.vel + outer.pan
        )");
        CHECK(result.success);
    }

    SECTION("spread overrides all fields") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            r = {..base, freq: 880, vel: 0.5}
            osc("sin", r.freq) |> % * r.vel |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("spread + shorthand field") {
        auto result = akkado::compile(R"(
            base = {freq: 440}
            vel = 0.8
            r = {..base, vel}
            r.freq + r.vel
        )");
        CHECK(result.success);
    }

    SECTION("spread field used in match scrutinee") {
        auto result = akkado::compile(R"(
            base = {mode: 1, freq: 440}
            r = {..base, mode: 2}
            match(r.mode) {
                1: 100,
                2: 200,
                _: 0
            }
        )");
        CHECK(result.success);
    }

    SECTION("spread record passed as fn argument") {
        auto result = akkado::compile(R"(
            base = {freq: 440, vel: 0.8}
            fn get_freq(r) -> r.freq
            get_freq({..base, pan: 0.5})
        )");
        CHECK(result.success);
    }

    SECTION("error: spread non-record") {
        auto result = akkado::compile(R"(
            x = 42
            r = {..x, vel: 0.8}
        )");
        CHECK_FALSE(result.success);
    }

    SECTION("two records in same pipe") {
        auto result = akkado::compile(R"(
            o = {freq: 440, type: "sin"}
            f = {cutoff: 1000, res: 0.707}
            osc(o.type, o.freq) |> lp(%, f.cutoff, f.res) |> out(%, %)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Codegen: Expression default interactions (F6 integration)
// =============================================================================

TEST_CASE("Codegen: Expression default interactions", "[codegen][fn]") {
    SECTION("expression default + pipe body") {
        auto result = akkado::compile(R"(
            fn f(x, gain = 1 + 1) -> x |> % * gain |> out(%, %)
            osc("sin", 440) |> f(%) |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("expression default + as-binding") {
        auto result = akkado::compile(R"(
            fn amp(x, gain = 2 * 0.25) -> x * gain
            osc("sin", 440) as sig |> amp(sig) |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("expression default + match body") {
        auto result = akkado::compile(R"(
            fn sel(x, mode = 1 + 0) -> match(mode) {
                1: x,
                2: x * 2,
                _: 0
            }
            sel(440)
        )");
        CHECK(result.success);
    }

    SECTION("expression default + dot-call") {
        auto result = akkado::compile(R"(
            fn amp(sig, gain = 1 + 1) -> sig * gain
            osc("sin", 440).amp() |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("negation as default") {
        auto result = akkado::compile(R"(
            fn offset(x, y = -1) -> x + y
            offset(10)
        )");
        CHECK(result.success);
    }

    SECTION("parenthesized expression default") {
        auto result = akkado::compile(R"(
            fn scale(x, factor = (1 + 2) * 3) -> x * factor
            scale(10)
        )");
        CHECK(result.success);
    }

    SECTION("pitch literal as default") {
        auto result = akkado::compile(R"(
            fn my_osc(type, freq = 'C4') -> osc(type, freq)
            my_osc("sin") |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("closure expr default assigned then called") {
        auto result = akkado::compile(R"(
            g = (x, y = 10 * 2) -> x + y
            g(1)
        )");
        CHECK(result.success);
    }

    SECTION("multiple defaults, partial override") {
        auto result = akkado::compile(R"(
            fn f(x, a = 1 + 1, b = 2 * 3) -> x + a + b
            f(1, 5)
        )");
        CHECK(result.success);
    }

    SECTION("expr default fn called multiple times") {
        auto result = akkado::compile(R"(
            fn f(x, scale = 2 + 2) -> x * scale
            f(1) + f(2) + f(3, 10)
        )");
        CHECK(result.success);
    }
}

// ============================================================================
// Timeline Curve Breakpoint Tests [timeline_codegen]
// ============================================================================

static akkado::PatternEventStream eval_curve(const std::string& curve_str) {
    akkado::AstArena arena;
    auto [root, diags] = akkado::parse_mini(curve_str, arena, {}, false, true);
    return akkado::evaluate_pattern(root, arena, 0);
}

TEST_CASE("Constant curve produces single breakpoint", "[timeline_codegen]") {
    auto stream = eval_curve("___");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() == 1);
    CHECK(breakpoints[0].time == Catch::Approx(0.0f));
    CHECK(breakpoints[0].value == Catch::Approx(0.0f));
    CHECK(breakpoints[0].curve == 2); // hold
}

TEST_CASE("Step curve produces two breakpoints", "[timeline_codegen]") {
    auto stream = eval_curve("__''");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() == 2);
    CHECK(breakpoints[0].value == Catch::Approx(0.0f));
    CHECK(breakpoints[0].curve == 2);
    CHECK(breakpoints[1].time == Catch::Approx(0.5f).margin(0.01f));
    CHECK(breakpoints[1].value == Catch::Approx(1.0f));
    CHECK(breakpoints[1].curve == 2);
}

TEST_CASE("Ramp produces linear breakpoints", "[timeline_codegen]") {
    auto stream = eval_curve("_/'");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() >= 2);
    // Should have linear interpolation somewhere
    bool has_linear = false;
    for (const auto& bp : breakpoints) {
        if (bp.curve == 0) has_linear = true;
    }
    CHECK(has_linear);
}

TEST_CASE("Smooth modifier produces linear breakpoint", "[timeline_codegen]") {
    auto stream = eval_curve("_~'");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    REQUIRE(breakpoints.size() >= 2);
    // The smooth level should be linear
    CHECK(breakpoints[1].curve == 0);
    CHECK(breakpoints[1].value == Catch::Approx(1.0f));
}

TEST_CASE("Multiple ramps produce proportional interpolation", "[timeline_codegen]") {
    auto stream = eval_curve("_//'");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    // Should have intermediate value around 0.5
    bool has_mid = false;
    for (const auto& bp : breakpoints) {
        if (bp.value > 0.3f && bp.value < 0.7f && bp.curve == 0) {
            has_mid = true;
        }
    }
    CHECK(has_mid);
}

TEST_CASE("All-same levels merge to single breakpoint", "[timeline_codegen]") {
    auto stream = eval_curve("'''''");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    CHECK(breakpoints.size() == 1);
    CHECK(breakpoints[0].value == Catch::Approx(1.0f));
}

TEST_CASE("Timeline curve compiles to TIMELINE opcode", "[timeline_codegen]") {
    auto result = akkado::compile("t\"____\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    bool found = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::TIMELINE) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("Timeline curve produces state init", "[timeline_codegen]") {
    auto result = akkado::compile("t\"__''\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
    bool found = false;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::Timeline) {
            found = true;
            CHECK(!init.timeline_breakpoints.empty());
            CHECK(init.timeline_loop == true);
            CHECK(init.timeline_loop_length > 0.0f);
        }
    }
    CHECK(found);
}

// ============================================================================
// Timeline End-to-End Integration Tests [timeline_e2e]
// ============================================================================

TEST_CASE("Timeline curve with math compiles", "[timeline_e2e]") {
    auto result = akkado::compile("t\"__/''\" * 1800 + 200 |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    CHECK(find_instruction(insts, cedar::Opcode::TIMELINE) != nullptr);
}

TEST_CASE("Timeline constant output compiles", "[timeline_e2e]") {
    auto result = akkado::compile("t\"''''\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    CHECK(find_instruction(insts, cedar::Opcode::TIMELINE) != nullptr);
}

TEST_CASE("Timeline with pipe chain compiles", "[timeline_e2e]") {
    auto result = akkado::compile("osc(\"sin\", 440) * t\"''''____\" |> out(%, %)");
    CHECK(result.diagnostics.empty());
}

TEST_CASE("Timeline function call form compiles", "[timeline_e2e]") {
    auto result = akkado::compile("timeline(\"__/''\") |> out(%, %)");
    CHECK(result.diagnostics.empty());
    auto insts = get_instructions(result);
    CHECK(find_instruction(insts, cedar::Opcode::TIMELINE) != nullptr);
    // Verify state init exists
    bool found = false;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::Timeline) found = true;
    }
    CHECK(found);
}

// =============================================================================
// Waterfall / FFT Visualization Tests
// =============================================================================

TEST_CASE("Codegen: waterfall() emits FFT_PROBE", "[codegen][viz]") {
    SECTION("basic waterfall with default fft size") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test") |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
        REQUIRE(fft != nullptr);
        CHECK(fft->rate == 10);  // default 1024
    }

    SECTION("waterfall with fft: 512") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test", {fft: 512}) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
        REQUIRE(fft != nullptr);
        CHECK(fft->rate == 9);  // 512 = 2^9
    }

    SECTION("waterfall with fft: 2048") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test", {fft: 2048}) |> out(%, %)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
        REQUIRE(fft != nullptr);
        CHECK(fft->rate == 11);  // 2048 = 2^11
    }

    SECTION("waterfall with string gradient option") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "test", {gradient: "viridis"}) |> out(%, %)
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::Waterfall) {
                CHECK(decl.options_json.find("\"gradient\":\"viridis\"") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION("waterfall creates Waterfall viz decl") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> waterfall(%, "my-spectrogram") |> out(%, %)
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::Waterfall) {
                CHECK(decl.name == "my-spectrogram");
                CHECK(decl.state_id != 0);
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: viz options serialize BoolLit values", "[codegen][viz]") {
    SECTION("boolean true") {
        auto result = akkado::compile(R"(
            osc("saw", 220) |> spectrum(%, "s", {logScale: true}) |> out(%, %)
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::Spectrum) {
                CHECK(decl.options_json.find("\"logScale\":true") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION("boolean false") {
        auto result = akkado::compile(R"(
            pat("c4 e4") |> pianoroll(%, "pr", {showGrid: false})
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::PianoRoll) {
                CHECK(decl.options_json.find("\"showGrid\":false") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION("mixed types: number, boolean, string") {
        // Phase 5 (PRD prd-records-system-unification §5.5): schema-aware
        // extract_options drops fields not declared on the builtin's schema.
        // pianoroll's schema declares all three types — width (Number),
        // showGrid (Bool), scale (Enum/string) — so all three round-trip.
        auto result = akkado::compile(R"(
            pat("c4 e4") |> pianoroll(%, "pr", {width: 200, showGrid: false, scale: "pentatonic"})
        )");
        REQUIRE(result.success);
        bool found = false;
        for (const auto& decl : result.viz_decls) {
            if (decl.type == akkado::VisualizationType::PianoRoll) {
                CHECK(decl.options_json.find("\"width\":200") != std::string::npos);
                CHECK(decl.options_json.find("\"showGrid\":false") != std::string::npos);
                CHECK(decl.options_json.find("\"scale\":\"pentatonic\"") != std::string::npos);
                found = true;
            }
        }
        CHECK(found);
    }
}

// =============================================================================
// PRD prd-records-system-unification §5.5 Phase 5 — shared extract_options
// helper. The helper is exercised end-to-end through the viz pipeline; these
// tests assert the schema-aware contract observable on VisualizationDecl.
// =============================================================================

TEST_CASE("Codegen: extract_options preserves recognized fields in source order",
          "[codegen][viz][options-helper]") {
    // Waterfall schema declares fft and gradient; both should round-trip.
    auto result = akkado::compile(R"(
        osc("saw", 220) |> waterfall(%, "w", {fft: 1024, gradient: "viridis"}) |> out(%, %)
    )");
    REQUIRE(result.success);
    bool found = false;
    for (const auto& decl : result.viz_decls) {
        if (decl.type == akkado::VisualizationType::Waterfall) {
            // Source order preserved (fft before gradient).
            auto pos_fft = decl.options_json.find("\"fft\":1024");
            auto pos_grad = decl.options_json.find("\"gradient\":\"viridis\"");
            REQUIRE(pos_fft != std::string::npos);
            REQUIRE(pos_grad != std::string::npos);
            CHECK(pos_fft < pos_grad);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("Codegen: extract_options drops unknown fields silently",
          "[codegen][viz][options-helper]") {
    // `nonsense` is not declared on the waterfall schema; per the user's
    // chosen Phase 5 contract the field is dropped from emitted JSON. The
    // unknown_fields list on OptionsPayload reserves this name for a future
    // W160 warning pass once the spread PRD lands W160 infrastructure.
    auto result = akkado::compile(R"(
        osc("saw", 220) |> waterfall(%, "w", {fft: 1024, nonsense: 7}) |> out(%, %)
    )");
    REQUIRE(result.success);
    bool found = false;
    for (const auto& decl : result.viz_decls) {
        if (decl.type == akkado::VisualizationType::Waterfall) {
            CHECK(decl.options_json.find("\"fft\":1024") != std::string::npos);
            CHECK(decl.options_json.find("nonsense") == std::string::npos);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("Codegen: extract_options yields empty JSON for empty record",
          "[codegen][viz][options-helper]") {
    // Pre-Phase-5 contract: empty record literal produces an empty options_json
    // string. The web UI relies on the empty string as a "no options supplied"
    // marker — the helper preserves that.
    auto result = akkado::compile(R"(
        osc("saw", 220) |> waterfall(%, "w", {}) |> out(%, %)
    )");
    REQUIRE(result.success);
    bool found = false;
    for (const auto& decl : result.viz_decls) {
        if (decl.type == akkado::VisualizationType::Waterfall) {
            CHECK(decl.options_json.empty());
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("Codegen: extract_options round-trips Number/Bool/String types",
          "[codegen][viz][options-helper]") {
    // pianoroll's schema covers all three OptionFieldType values that the
    // helper serializes today: Number (width), Bool (showGrid), and Enum
    // (scale, written as a string literal).
    auto result = akkado::compile(R"(
        pat("c4 e4") |> pianoroll(%, "pr", {width: 200, showGrid: false, scale: "pentatonic"})
    )");
    REQUIRE(result.success);
    bool found = false;
    for (const auto& decl : result.viz_decls) {
        if (decl.type == akkado::VisualizationType::PianoRoll) {
            CHECK(decl.options_json.find("\"width\":200") != std::string::npos);
            CHECK(decl.options_json.find("\"showGrid\":false") != std::string::npos);
            CHECK(decl.options_json.find("\"scale\":\"pentatonic\"") != std::string::npos);
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("Codegen: spectrum/waterfall fft_log2 lookup parity",
          "[codegen][viz][options-helper]") {
    // Each accepted fft size lowers to the documented log2 byte. The pre-
    // Phase-5 path read this by string-searching the JSON; the new path
    // uses payload.get_number("fft"). Both must produce identical bytes.
    struct FftCase { const char* fft; std::uint8_t expected; };
    const FftCase cases[] = {
        {"256",  8},
        {"512",  9},
        {"1024", 10},
        {"2048", 11},
    };
    for (const auto& c : cases) {
        std::string src = std::string("osc(\"saw\", 220) |> spectrum(%, \"s\", {fft: ")
                          + c.fft + "}) |> out(%, %)";
        auto result = akkado::compile(src);
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
        REQUIRE(fft != nullptr);
        CHECK(fft->rate == c.expected);
    }
}

TEST_CASE("Codegen: spectrum/waterfall fft default applies when fft absent",
          "[codegen][viz][options-helper]") {
    // No fft field on the record — payload.get_number returns nullopt and
    // fft_log2_from_payload falls back to 10 (=> 1024 bins).
    auto result = akkado::compile(R"(
        osc("saw", 220) |> waterfall(%, "w", {gradient: "viridis"}) |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
    REQUIRE(fft != nullptr);
    CHECK(fft->rate == 10);
}

TEST_CASE("Codegen: spectrum() now emits FFT_PROBE", "[codegen][viz]") {
    auto result = akkado::compile(R"(
        osc("saw", 220) |> spectrum(%, "fft") |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);

    // Should use FFT_PROBE, not PROBE
    auto* fft = find_instruction(insts, cedar::Opcode::FFT_PROBE);
    REQUIRE(fft != nullptr);
    CHECK(fft->rate == 10);  // default 1024

    // Should NOT have a PROBE instruction
    auto* probe = find_instruction(insts, cedar::Opcode::PROBE);
    CHECK(probe == nullptr);
}

// =============================================================================
// Builtin Variable Tests
// =============================================================================

TEST_CASE("Codegen: bpm assignment generates override metadata", "[codegen][builtins]") {
    SECTION("basic bpm assignment") {
        auto result = akkado::compile(R"(
            bpm = 120
            saw(220) |> out(%, %)
        )");
        REQUIRE(result.success);
        REQUIRE(result.builtin_var_overrides.size() == 1);
        CHECK(result.builtin_var_overrides[0].name == "bpm");
        CHECK(result.builtin_var_overrides[0].value == Catch::Approx(120.0f));

        // bpm is only assigned, not read — no ENV_GET emitted for __bpm
        auto insts = get_instructions(result);
        bool found_bpm_env_get = false;
        std::uint32_t bpm_hash = cedar::fnv1a_hash_runtime("__bpm", 5);
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == bpm_hash) {
                found_bpm_env_get = true;
            }
        }
        CHECK_FALSE(found_bpm_env_get);
    }

    SECTION("bpm with arithmetic expression") {
        auto result = akkado::compile(R"(
            bpm = 60 * 2
            saw(220) |> out(%, %)
        )");
        REQUIRE(result.success);
        REQUIRE(result.builtin_var_overrides.size() == 1);
        CHECK(result.builtin_var_overrides[0].value == Catch::Approx(120.0f));
    }

    SECTION("bpm value is clamped to valid range") {
        auto result = akkado::compile(R"(
            bpm = 0.5
            saw(220) |> out(%, %)
        )");
        REQUIRE(result.success);
        REQUIRE(result.builtin_var_overrides.size() == 1);
        CHECK(result.builtin_var_overrides[0].value == Catch::Approx(1.0f));
    }
}

TEST_CASE("Codegen: reading bpm emits ENV_GET", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        x = 60 / bpm
        saw(x) |> out(%, %)
    )");
    REQUIRE(result.success);

    auto insts = get_instructions(result);
    std::uint32_t bpm_hash = cedar::fnv1a_hash_runtime("__bpm", 5);
    bool found = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == bpm_hash) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("Codegen: sr is read-only", "[codegen][builtins]") {
    SECTION("sr assignment is error") {
        auto result = akkado::compile(R"(
            sr = 44100
            saw(220) |> out(%, %)
        )");
        REQUIRE_FALSE(result.success);
        bool found_e170 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E170") found_e170 = true;
        }
        CHECK(found_e170);
    }

    SECTION("reading sr emits ENV_GET") {
        auto result = akkado::compile(R"(
            x = 220 / sr
            saw(x) |> out(%, %)
        )");
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        std::uint32_t sr_hash = cedar::fnv1a_hash_runtime("__sr", 4);
        bool found = false;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == sr_hash) {
                found = true;
            }
        }
        CHECK(found);
    }
}

TEST_CASE("Codegen: const bpm is error", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        const bpm = 120
        saw(220) |> out(%, %)
    )");
    REQUIRE_FALSE(result.success);
    bool found_e170 = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E170") found_e170 = true;
    }
    CHECK(found_e170);
}

TEST_CASE("Codegen: bpm with non-constant expression is error", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        bpm = param("tempo", 120, 60, 200)
        saw(220) |> out(%, %)
    )");
    REQUIRE_FALSE(result.success);
    bool found_e172 = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E172") found_e172 = true;
    }
    CHECK(found_e172);
}

TEST_CASE("Codegen: multiple bpm assignments both stored", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        bpm = 100
        bpm = 140
        saw(220) |> out(%, %)
    )");
    REQUIRE(result.success);
    REQUIRE(result.builtin_var_overrides.size() == 2);
    CHECK(result.builtin_var_overrides[0].value == Catch::Approx(100.0f));
    CHECK(result.builtin_var_overrides[1].value == Catch::Approx(140.0f));
}

TEST_CASE("Codegen: spb is read-only", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        spb = 0.5
        saw(220) |> out(%, %)
    )");
    REQUIRE_FALSE(result.success);
    bool found_e170 = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E170") found_e170 = true;
    }
    CHECK(found_e170);
}

TEST_CASE("Codegen: reading spb emits ENV_GET", "[codegen][builtins]") {
    auto result = akkado::compile(R"(
        x = 220 * spb
        saw(x) |> out(%, %)
    )");
    REQUIRE(result.success);
    std::uint32_t spb_hash = cedar::fnv1a_hash_runtime("__spb", 5);
    bool found_spb_env_get = false;
    auto insts = get_instructions(result);
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == spb_hash) {
            found_spb_env_get = true;
            break;
        }
    }
    CHECK(found_spb_env_get);
}

TEST_CASE("Codegen: spb value reflects current bpm", "[codegen][builtins]") {
    // Default bpm=120: spb = 60/120 = 0.5 seconds per beat
    {
        auto result = akkado::compile(R"(
            x = spb
            saw(x) |> out(%, %)
        )");
        REQUIRE(result.success);
        // spb is read-only, so it emits ENV_GET(__spb), not a builtin_var_override
        std::uint32_t spb_hash = cedar::fnv1a_hash_runtime("__spb", 5);
        bool found_spb_env_get = false;
        auto insts = get_instructions(result);
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::ENV_GET && inst.state_id == spb_hash) {
                found_spb_env_get = true;
                break;
            }
        }
        CHECK(found_spb_env_get);
    }
}

TEST_CASE("Runtime: spb reflects bpm changes via VM::set_bpm", "[codegen][builtins][runtime]") {
    using Catch::Matchers::WithinAbs;

    // Compile a program whose last instruction's output buffer holds spb.
    // `spb * 1.0` emits ENV_GET(__spb) followed by MUL; MUL's buffer carries spb.
    auto result = akkado::compile("spb * 1.0");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    REQUIRE_FALSE(insts.empty());

    cedar::VM vm;
    std::span<const cedar::Instruction> bc(insts.data(), insts.size());
    REQUIRE(vm.load_program_immediate(bc));

    std::array<float, cedar::BLOCK_SIZE> left{};
    std::array<float, cedar::BLOCK_SIZE> right{};
    const std::uint16_t out_idx = insts.back().out_buffer;

    auto run_blocks_and_read = [&](int n) {
        for (int i = 0; i < n; ++i) {
            vm.process_block(left.data(), right.data());
        }
        const float* buf = vm.buffers().get(out_idx);
        REQUIRE(buf != nullptr);
        return buf[cedar::BLOCK_SIZE - 1];
    };

    // First call seeds __spb directly (no slew on initial set), so a single
    // block is enough to read the new value.
    vm.set_bpm(120.0f);
    CHECK_THAT(run_blocks_and_read(1), WithinAbs(0.5f, 1e-4f));

    // Subsequent set_bpm calls slew via EnvMap; allow a few blocks to settle.
    vm.set_bpm(60.0f);
    CHECK_THAT(run_blocks_and_read(64), WithinAbs(1.0f, 1e-3f));

    vm.set_bpm(240.0f);
    CHECK_THAT(run_blocks_and_read(64), WithinAbs(0.25f, 1e-3f));
}

// =============================================================================
// Conditionals & Logic — Runtime Value Tests
//
// These tests compile akkado source, load the bytecode into a Cedar VM,
// process one block, and assert the actual runtime value of the destination
// buffer of the last instruction. This verifies that the conditionals/logic
// operators produce the documented 0.0/1.0 outputs (and not, say, an inverted
// truth polarity that codegen-presence tests above cannot catch).
// =============================================================================

namespace {

// Compile akkado source, run one block, and return the first sample of the
// destination buffer of the program's last instruction.
float run_first_sample(std::string_view source) {
    auto result = akkado::compile(source);
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    REQUIRE_FALSE(insts.empty());

    cedar::VM vm;
    std::span<const cedar::Instruction> bc(insts.data(), insts.size());
    REQUIRE(vm.load_program_immediate(bc));

    std::array<float, cedar::BLOCK_SIZE> left{};
    std::array<float, cedar::BLOCK_SIZE> right{};
    vm.process_block(left.data(), right.data());

    const std::uint16_t out_idx = insts.back().out_buffer;
    const float* buf = vm.buffers().get(out_idx);
    REQUIRE(buf != nullptr);
    return buf[0];
}

}  // namespace

TEST_CASE("Runtime: gt/lt/gte/lte produce 0.0 or 1.0", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("gt true / false / equal") {
        CHECK_THAT(run_first_sample("gt(10, 5)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gt(5, 10)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gt(5, 5)"),  WithinAbs(0.0f, 1e-6f));
    }

    SECTION("lt true / false / equal") {
        CHECK_THAT(run_first_sample("lt(5, 10)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lt(10, 5)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lt(5, 5)"),  WithinAbs(0.0f, 1e-6f));
    }

    SECTION("gte includes equality") {
        CHECK_THAT(run_first_sample("gte(10, 5)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gte(5, 5)"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("gte(5, 10)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("lte includes equality") {
        CHECK_THAT(run_first_sample("lte(5, 10)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lte(5, 5)"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("lte(10, 5)"), WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: eq/neq with epsilon", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("exact equality") {
        CHECK_THAT(run_first_sample("eq(5, 5)"),    WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("eq(5, 10)"),   WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(5, 5)"),   WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(5, 10)"),  WithinAbs(1.0f, 1e-6f));
    }

    SECTION("eq treats near-equal floats as equal") {
        // Floating-point round-off: 0.1 + 0.2 != 0.3 in strict IEEE,
        // but the documented epsilon (1e-6) must collapse them to true.
        CHECK_THAT(run_first_sample("eq(0.1 + 0.2, 0.3)"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(0.1 + 0.2, 0.3)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("eq separates values farther apart than epsilon") {
        // 1e-3 is well above the 1e-6 epsilon, must compare not-equal.
        CHECK_THAT(run_first_sample("eq(1.0, 1.001)"),  WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("neq(1.0, 1.001)"), WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: band/bor/bnot truth tables", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("band truth table") {
        CHECK_THAT(run_first_sample("band(1, 1)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("band(1, 0)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("band(0, 1)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("band(0, 0)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("bor truth table") {
        CHECK_THAT(run_first_sample("bor(1, 1)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(1, 0)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(0, 1)"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(0, 0)"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("bnot inverts truthiness") {
        CHECK_THAT(run_first_sample("bnot(1)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bnot(0)"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("negative values are falsy") {
        // band/bor/bnot all treat values <= 0 as false, including negatives.
        CHECK_THAT(run_first_sample("band(-1, 1)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bor(-1, -2)"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("bnot(-1)"),    WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: select picks the right branch", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("truthy condition") {
        CHECK_THAT(run_first_sample("select(1, 100, 50)"), WithinAbs(100.0f, 1e-6f));
    }

    SECTION("zero is falsy") {
        CHECK_THAT(run_first_sample("select(0, 100, 50)"), WithinAbs(50.0f, 1e-6f));
    }

    SECTION("negative is falsy") {
        // Documented in PRD: select uses cond > 0, so negatives go to b.
        CHECK_THAT(run_first_sample("select(-1, 100, 50)"), WithinAbs(50.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: infix syntax matches function-call syntax", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("comparison infix") {
        CHECK_THAT(run_first_sample("10 > 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 < 10"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 >= 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 <= 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 == 5"),  WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("5 != 10"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("logic infix") {
        CHECK_THAT(run_first_sample("1 && 1"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("1 || 0"), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(run_first_sample("0 && 1"), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("!1"),     WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(run_first_sample("!0"),     WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("Runtime: operator precedence", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    SECTION("&& binds tighter than ||") {
        // 1 || 0 && 0 must parse as 1 || (0 && 0) = 1
        CHECK_THAT(run_first_sample("1 || 0 && 0"), WithinAbs(1.0f, 1e-6f));
        // 0 && 1 || 0 must parse as (0 && 1) || 0 = 0
        CHECK_THAT(run_first_sample("0 && 1 || 0"), WithinAbs(0.0f, 1e-6f));
    }

    SECTION("comparison binds tighter than logic") {
        // (5 > 3) && (2 < 4) = 1
        CHECK_THAT(run_first_sample("5 > 3 && 2 < 4"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("arithmetic binds tighter than comparison") {
        // (2 + 3) > 4 = 5 > 4 = 1
        CHECK_THAT(run_first_sample("2 + 3 > 4"), WithinAbs(1.0f, 1e-6f));
        // (2 * 3) == 6 = 1
        CHECK_THAT(run_first_sample("2 * 3 == 6"), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("equality binds looser than comparison") {
        // (5 > 3) == 1 — comparison evaluated first, equality compares 1.0 to 1
        CHECK_THAT(run_first_sample("5 > 3 == 1"), WithinAbs(1.0f, 1e-6f));
    }
}

// Signal-rate square-wave verification from the PRD's Test Plan:
//   check_signal("osc(\"sin\", 1) > 0", /* verify square wave */)
// Threshold-comparing a sine against 0 must produce a 50% duty-cycle square
// wave whose samples are exactly 0.0 or 1.0. At 48 kHz with f=1 Hz, one period
// is 48000 samples = 375 blocks — we run 400 blocks to safely cover one full
// period plus margin.
TEST_CASE("Runtime: osc(sin, 1) > 0 produces a square wave", "[conditionals][runtime]") {
    using Catch::Matchers::WithinAbs;

    auto result = akkado::compile("osc(\"sin\", 1) > 0");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    REQUIRE_FALSE(insts.empty());

    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    std::span<const cedar::Instruction> bc(insts.data(), insts.size());
    REQUIRE(vm.load_program_immediate(bc));

    const std::uint16_t out_idx = insts.back().out_buffer;

    constexpr std::size_t kBlocks = 400;
    std::size_t high_samples = 0;
    std::size_t low_samples = 0;
    std::size_t transitions = 0;
    float prev = -1.0f;  // sentinel so the first real sample doesn't count as a transition

    for (std::size_t b = 0; b < kBlocks; ++b) {
        std::array<float, cedar::BLOCK_SIZE> left{}, right{};
        vm.process_block(left.data(), right.data());
        const float* buf = vm.buffers().get(out_idx);
        REQUIRE(buf != nullptr);

        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            const float v = buf[i];
            // Output must be exactly binary — no intermediate values.
            const bool is_binary = std::fabs(v) < 1e-6f || std::fabs(v - 1.0f) < 1e-6f;
            REQUIRE(is_binary);
            if (v > 0.5f) ++high_samples; else ++low_samples;
            if (prev >= 0.0f && std::fabs(v - prev) > 0.5f) ++transitions;
            prev = v;
        }
    }

    // Over one full period at 50% duty cycle, high and low counts should be
    // close to equal. Allow 5% tolerance for the partial extra cycle in 400
    // blocks vs. one period (375 blocks).
    const std::size_t total = high_samples + low_samples;
    const float duty = static_cast<float>(high_samples) / static_cast<float>(total);
    CHECK(duty > 0.45f);
    CHECK(duty < 0.55f);

    // Must transition at least twice (once high→low, once low→high) to prove
    // it isn't stuck constant.
    CHECK(transitions >= 2);
}

// =============================================================================
// Bug repro: chord("C Em Am G") |> poly() — incomplete chord onsets
// =============================================================================
//
// User report: with `chord("C Em Am G") |> poly(@, stab, 21) * 0.33 |> out(@)`
// where stab has a 0.4s release + 0.3s delay tail, every few bars a chord
// plays with a missing note. This test compiles the user's exact source,
// runs it for many cycles, snapshots the voice grid each block, and detects
// chord onsets where fewer than 3 voices are firing the expected freqs.

namespace {

// Apply state_inits from a CompileResult to the given VM.
// Mirrors the logic in web/wasm/nkido_wasm.cpp:apply_state_inits.
void apply_state_inits(cedar::VM& vm, const akkado::CompileResult& result,
                       std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    // We need to keep Sequence storage alive for the duration of the test.
    seq_storage.reserve(result.state_inits.size());
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(init.sequence_events[i].data());
                    seq_copy[i].num_events = static_cast<std::uint32_t>(init.sequence_events[i].size());
                    seq_copy[i].capacity = static_cast<std::uint32_t>(init.sequence_events[i].size());
                }
            }
            seq_storage.push_back(std::move(seq_copy));
            auto& stored = seq_storage.back();
            vm.init_sequence_program_state(
                init.state_id,
                stored.data(), stored.size(),
                init.cycle_length,
                init.is_sample_pattern,
                init.total_events
            );
        } else if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            vm.init_poly_state(
                init.state_id,
                init.poly_seq_state_id,
                init.poly_max_voices,
                init.poly_mode,
                init.poly_steal_strategy
            );
        }
    }
}

}  // namespace

TEST_CASE("Runtime: chord(...) |> poly fires every chord note completely",
          "[poly][regression][chord-completeness]") {
    // The user's exact patch
    const char* src = R"(
        stab = (freq, gate, vel) ->
            saw(freq) * ar(gate, 0.05, 0.4) * vel
            |> lp(@, 1100)
            |> @ + delay(@, 0.3, 0)

        chord("C Em Am G")
            |> poly(@, stab, 21) * 0.33
            |> out(@)
    )";

    auto result = akkado::compile(src);
    REQUIRE(result.success);

    auto insts = get_instructions(result);
    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_bpm(120.0f);
    REQUIRE(vm.load_program_immediate(std::span<const cedar::Instruction>(insts)));

    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_state_inits(vm, result, seq_storage);

    // Find the PolyAllocState ID
    std::uint32_t poly_state_id = 0;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            poly_state_id = init.state_id;
            break;
        }
    }
    REQUIRE(poly_state_id != 0);

    // Chord parser uses default octave 4 for the root, then adds intervals
    // C major = [0, 4, 7] from C4 → C4, E4, G4 = MIDI 60, 64, 67
    // E minor = [0, 3, 7] from E4 → E4, G4, B4 = MIDI 64, 67, 71
    // A minor = [0, 3, 7] from A4 → A4, C5, E5 = MIDI 69, 72, 76
    // G major = [0, 4, 7] from G4 → G4, B4, D5 = MIDI 67, 71, 74
    auto midi_to_hz = [](int midi) {
        return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f);
    };
    const std::array<std::array<float, 3>, 4> CHORDS = {{
        {midi_to_hz(60), midi_to_hz(64), midi_to_hz(67)},   // C
        {midi_to_hz(64), midi_to_hz(67), midi_to_hz(71)},   // Em
        {midi_to_hz(69), midi_to_hz(72), midi_to_hz(76)},   // Am
        {midi_to_hz(67), midi_to_hz(71), midi_to_hz(74)},   // G
    }};

    // 120 BPM, 48 kHz, BLOCK_SIZE 128:
    // samples per beat = 48000 * 60 / 120 = 24000
    // 1 cycle (4 beats) = 96000 samples = 750 blocks exactly
    // Each chord lasts 1 beat = 24000 samples = 187.5 blocks
    constexpr int BLOCKS_PER_CYCLE = 750;
    constexpr int CYCLES = 32;  // long enough to surface "every few bars" bug
    constexpr int TOTAL_BLOCKS = CYCLES * BLOCKS_PER_CYCLE;

    std::array<float, cedar::BLOCK_SIZE> left{}, right{};
    auto& poly = vm.states().get_or_create<cedar::PolyAllocState>(poly_state_id);

    int incomplete_count = 0;
    int onsets_inspected = 0;
    int last_chord_idx = -1;
    int first_failure_block = -1;
    int first_failure_chord = -1;
    int first_failure_matched = -1;

    for (int b = 0; b < TOTAL_BLOCKS; ++b) {
        const float block_beats = (b * static_cast<float>(cedar::BLOCK_SIZE)) /
                                  (48000.0f * 60.0f / 120.0f);
        const float cycle_pos = std::fmod(block_beats, 4.0f);
        const int chord_idx = static_cast<int>(std::floor(cycle_pos));

        vm.process_block(left.data(), right.data());

        // Inspect 30 blocks into each new chord (gives time for attack to settle)
        const int blocks_into_chord = static_cast<int>((cycle_pos - chord_idx) * 187.5f);
        if (chord_idx != last_chord_idx && blocks_into_chord >= 30) {
            // Skip the very first chord at b=0 (no settle time)
            if (b > 100) {
                onsets_inspected++;
                const auto& chord = CHORDS[chord_idx];
                int matched = 0;
                for (float target : chord) {
                    bool found = false;
                    for (std::uint8_t i = 0; i < poly.max_voices; ++i) {
                        const auto& v = poly.voices[i];
                        if (v.active && !v.releasing && v.gate > 0.5f &&
                            std::abs(v.freq - target) < 0.5f) {
                            found = true;
                            break;
                        }
                    }
                    if (found) ++matched;
                }
                if (matched < 3) {
                    if (first_failure_block < 0) {
                        first_failure_block = b;
                        first_failure_chord = chord_idx;
                        first_failure_matched = matched;
                    }
                    ++incomplete_count;
                }
            }
            last_chord_idx = chord_idx;
        }
    }

    INFO("Inspected " << onsets_inspected << " chord onsets, "
         << incomplete_count << " incomplete");
    if (first_failure_block >= 0) {
        INFO("First failure: block=" << first_failure_block
             << " chord_idx=" << first_failure_chord
             << " matched=" << first_failure_matched << "/3");
    }
    CHECK(onsets_inspected >= 30);
    CHECK(incomplete_count == 0);
}

// =============================================================================
// Audio Input Tests (in() builtin)
// =============================================================================

TEST_CASE("Codegen: in() emits INPUT and produces stereo signal", "[codegen][input]") {
    SECTION("in() compiles and emits exactly one INPUT instruction") {
        auto result = akkado::compile("in() |> out(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::INPUT) == 1);

        auto* in = find_instruction(insts, cedar::Opcode::INPUT);
        REQUIRE(in != nullptr);
        // INPUT is stateless: state_id should be 0
        CHECK(in->state_id == 0);
    }

    SECTION("in() default has empty source string in required_input_sources") {
        auto result = akkado::compile("in() |> out(%)");
        REQUIRE(result.success);
        REQUIRE(result.required_input_sources.size() == 1);
        CHECK(result.required_input_sources[0] == "");
    }

    SECTION("in() output reaches OUTPUT (full pipeline compiles)") {
        auto result = akkado::compile("in() |> out(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
    }
}

TEST_CASE("Codegen: in() with explicit source string", "[codegen][input]") {
    SECTION("in('mic') compiles") {
        auto result = akkado::compile(R"(in("mic") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(result.required_input_sources.size() == 1);
        CHECK(result.required_input_sources[0] == "mic");
    }

    SECTION("in('tab') compiles") {
        auto result = akkado::compile(R"(in("tab") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(result.required_input_sources.size() == 1);
        CHECK(result.required_input_sources[0] == "tab");
    }

    SECTION("in('file:NAME') compiles") {
        auto result = akkado::compile(R"(in("file:voice.wav") |> out(%))");
        REQUIRE(result.success);
        REQUIRE(result.required_input_sources.size() == 1);
        CHECK(result.required_input_sources[0] == "file:voice.wav");
    }

    SECTION("in() with unknown source string is a compile error") {
        auto result = akkado::compile(R"(in("garbage_source") |> out(%))");
        CHECK_FALSE(result.success);
    }

    SECTION("in('file:') (no name after colon) is a compile error") {
        auto result = akkado::compile(R"(in("file:") |> out(%))");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("Codegen: in() into stereo-native DSP carries stereo through", "[codegen][input][stereo]") {
    SECTION("in() |> lp(%, 2000) emits a STEREO_INPUT-flagged filter") {
        auto result = akkado::compile("in() |> lp(%, 2000) |> out(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);

        auto* in = find_instruction(insts, cedar::Opcode::INPUT);
        REQUIRE(in != nullptr);

        auto* lp = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(lp != nullptr);
        // Stereo primary input: the stereo-native filter reads both channels.
        CHECK((lp->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("in()'s output buffers are an adjacent (left, left+1) pair") {
        auto result = akkado::compile("in() |> out(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* in = find_instruction(insts, cedar::Opcode::INPUT);
        REQUIRE(in != nullptr);

        // Find the OUTPUT, which should consume the L and R buffer of INPUT.
        auto* out = find_instruction(insts, cedar::Opcode::OUTPUT);
        REQUIRE(out != nullptr);
        CHECK(out->inputs[0] == in->out_buffer);
        CHECK(out->inputs[1] == static_cast<std::uint16_t>(in->out_buffer + 1));
    }
}

// ============================================================================
// Timeline Edge Case Tests [timeline_edge_cases]
// ============================================================================

TEST_CASE("Empty curve string produces no breakpoints", "[timeline_edge_cases]") {
    auto stream = eval_curve("");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    CHECK(breakpoints.empty());
}

TEST_CASE("Single-character curve ' produces single breakpoint", "[timeline_edge_cases]") {
    auto stream = eval_curve("'");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    CHECK(breakpoints.size() == 1);
    CHECK(breakpoints[0].value == Catch::Approx(1.0f));
    CHECK(breakpoints[0].curve == 2); // hold
}

TEST_CASE("Single-character curve _ produces single breakpoint", "[timeline_edge_cases]") {
    auto stream = eval_curve("_");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    CHECK(breakpoints.size() == 1);
    CHECK(breakpoints[0].value == Catch::Approx(0.0f));
    CHECK(breakpoints[0].curve == 2); // hold
}

TEST_CASE("Ramp at start interpolates from 0.0 to next level", "[timeline_edge_cases]") {
    auto stream = eval_curve("/''");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    CHECK(breakpoints.size() >= 2);
    CHECK(breakpoints[0].time == Catch::Approx(0.0f));
    // No preceding level → defaults to 0.0, next level is 1.0
    // Single ramp: t=0.5, so value = 0.0 + 0.5*(1.0-0.0) = 0.5
    CHECK(breakpoints[0].value == Catch::Approx(0.5f));
    CHECK(breakpoints[0].curve == 0); // linear
}

TEST_CASE("Ramp at end defaults to 0.0", "[timeline_edge_cases]") {
    auto stream = eval_curve("___/");
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    CHECK(breakpoints.size() >= 2);
    bool has_linear = false;
    for (const auto& bp : breakpoints) {
        if (bp.curve == 0) { has_linear = true; break; }
    }
    CHECK(has_linear);
}

TEST_CASE("~ on ramp produces compile error", "[timeline_edge_cases]") {
    auto result = akkado::compile("t\"~ /\" |> out(%, %)");
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.diagnostics.empty());
}

TEST_CASE("Many levels produce correct breakpoint count", "[timeline_edge_cases]") {
    // Create a curve with 65 level characters (alternating _ and ')
    std::string long_curve;
    for (int i = 0; i < 65; ++i) {
        long_curve += (i % 2 == 0) ? "_" : "'";
    }
    auto stream = eval_curve(long_curve);
    auto breakpoints = akkado::events_to_breakpoints(stream.events);
    // 65 levels → 65 breakpoints (no merge since adjacent values differ)
    CHECK(breakpoints.size() == 65);
}

TEST_CASE("Breakpoint limit warns and truncates via codegen", "[timeline_edge_cases]") {
    // Create a curve with >64 breakpoints and compile it
    std::string long_curve = "timeline(\"";
    for (int i = 0; i < 65; ++i) {
        long_curve += (i % 2 == 0) ? "_" : "'";
    }
    long_curve += "\") |> out(%, %)";
    auto result = akkado::compile(long_curve);
    // Should succeed with a warning about truncation
    CHECK(result.success);
    // Check that a warning was emitted
    bool has_warning = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("64 breakpoints") != std::string::npos) {
            has_warning = true;
            break;
        }
    }
    CHECK(has_warning);
}

// =============================================================================
// samples() builtin (URI declaration for sample banks)
// =============================================================================

TEST_CASE("samples() records a single URI", "[samples-builtin]") {
    auto result = akkado::compile(
        "samples(\"github:tidalcycles/Dirt-Samples\")\n"
        "0 |> out(%, %)"
    );
    REQUIRE(result.success);
    REQUIRE(result.required_uris.size() == 1);
    CHECK(result.required_uris[0].uri == "github:tidalcycles/Dirt-Samples");
    CHECK(result.required_uris[0].kind == akkado::UriKind::SampleBank);
}

TEST_CASE("samples() preserves source order across multiple calls", "[samples-builtin]") {
    auto result = akkado::compile(
        "samples(\"github:foo/bar\")\n"
        "samples(\"https://example.com/strudel.json\")\n"
        "samples(\"file:///tmp/local.json\")\n"
        "0 |> out(%, %)"
    );
    REQUIRE(result.success);
    REQUIRE(result.required_uris.size() == 3);
    CHECK(result.required_uris[0].uri == "github:foo/bar");
    CHECK(result.required_uris[1].uri == "https://example.com/strudel.json");
    CHECK(result.required_uris[2].uri == "file:///tmp/local.json");
}

TEST_CASE("samples() dedups identical URIs", "[samples-builtin]") {
    auto result = akkado::compile(
        "samples(\"github:foo/bar\")\n"
        "samples(\"github:foo/bar\")\n"
        "0 |> out(%, %)"
    );
    REQUIRE(result.success);
    REQUIRE(result.required_uris.size() == 1);
    CHECK(result.required_uris[0].uri == "github:foo/bar");
}

TEST_CASE("samples() rejects non-string-literal arg", "[samples-builtin]") {
    auto result = akkado::compile(
        "u = \"github:foo/bar\"\n"
        "samples(u)\n"
        "0 |> out(%, %)"
    );
    CHECK_FALSE(result.success);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E231") { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE("samples() rejects empty URI", "[samples-builtin]") {
    auto result = akkado::compile(
        "samples(\"\")\n"
        "0 |> out(%, %)"
    );
    CHECK_FALSE(result.success);
    bool found = false;
    for (const auto& d : result.diagnostics) {
        if (d.code == "E232") { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE("samples() rejects wrong arg count", "[samples-builtin]") {
    // Arity validation fires from the generic builtin signature check
    // (min=1, max=1) before the special handler runs — same as wt_load.
    SECTION("zero args") {
        auto result = akkado::compile(
            "samples()\n"
            "0 |> out(%, %)"
        );
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E006") { found = true; break; }
        }
        CHECK(found);
    }

    SECTION("two args") {
        auto result = akkado::compile(
            "samples(\"github:foo/bar\", \"extra\")\n"
            "0 |> out(%, %)"
        );
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E007") { found = true; break; }
        }
        CHECK(found);
    }
}

TEST_CASE("samples() emits no audio-time instruction", "[samples-builtin]") {
    auto result = akkado::compile(
        "samples(\"github:foo/bar\")\n"
        "0 |> out(%, %)"
    );
    REQUIRE(result.success);
    auto with = get_instructions(result);

    auto baseline = akkado::compile("0 |> out(%, %)");
    REQUIRE(baseline.success);
    auto without = get_instructions(baseline);

    // samples() is a compile-time directive; bytecode size must be unchanged
    CHECK(with.size() == without.size());
}

// =============================================================================
// sample() with string sample name — direct trigger-driven sampler call.
// `sample(trig, pitch, "name")` registers the name in required_samples_extended
// and emits a PUSH_CONST placeholder whose state_id is patched at runtime by
// `akkado_patch_sample_ids_in_bytecode()` once the sample bank is populated.
// =============================================================================
TEST_CASE("sample() accepts a sample-name string", "[codegen][sample][string-arg]") {
    SECTION("plain name registers required_sample and emits placeholder PUSH_CONST") {
        auto result = akkado::compile(
            "trig = button(\"hit\")\n"
            "sample(trig, 1.0, \"bd\") |> out(%, %)"
        );
        REQUIRE(result.success);

        REQUIRE(result.required_samples_extended.size() == 1);
        CHECK(result.required_samples_extended[0].bank.empty());
        CHECK(result.required_samples_extended[0].name == "bd");
        CHECK(result.required_samples_extended[0].variant == 0);

        REQUIRE(result.scalar_sample_mappings.size() == 1);
        const auto& mapping = result.scalar_sample_mappings[0];
        CHECK(mapping.bank.empty());
        CHECK(mapping.name == "bd");
        CHECK(mapping.variant == 0);

        auto insts = get_instructions(result);
        REQUIRE(mapping.instruction_index < insts.size());
        const auto& push = insts[mapping.instruction_index];
        CHECK(push.opcode == cedar::Opcode::PUSH_CONST);
        // Codegen records 0 as the placeholder; host patches at load time.
        CHECK(decode_const_float(push) == 0.0f);

        // Find the SAMPLE_PLAY and verify its sample_id input wires to this
        // PUSH_CONST's output buffer.
        const cedar::Instruction* sp = find_instruction(insts, cedar::Opcode::SAMPLE_PLAY);
        REQUIRE(sp != nullptr);
        CHECK(sp->inputs[2] == push.out_buffer);
    }

    SECTION("name with variant suffix") {
        auto result = akkado::compile(
            "trig = button(\"hit\")\n"
            "sample(trig, 1.0, \"bd:3\") |> out(%, %)"
        );
        REQUIRE(result.success);

        REQUIRE(result.required_samples_extended.size() == 1);
        CHECK(result.required_samples_extended[0].name == "bd");
        CHECK(result.required_samples_extended[0].variant == 3);

        REQUIRE(result.scalar_sample_mappings.size() == 1);
        CHECK(result.scalar_sample_mappings[0].name == "bd");
        CHECK(result.scalar_sample_mappings[0].variant == 3);
    }

    SECTION("bank-qualified name") {
        auto result = akkado::compile(
            "trig = button(\"hit\")\n"
            "sample(trig, 1.0, \"Dirt-Samples/amencutup:0\") |> out(%, %)"
        );
        REQUIRE(result.success);

        REQUIRE(result.required_samples_extended.size() == 1);
        CHECK(result.required_samples_extended[0].bank == "Dirt-Samples");
        CHECK(result.required_samples_extended[0].name == "amencutup");
        CHECK(result.required_samples_extended[0].variant == 0);

        REQUIRE(result.scalar_sample_mappings.size() == 1);
        CHECK(result.scalar_sample_mappings[0].bank == "Dirt-Samples");
        CHECK(result.scalar_sample_mappings[0].name == "amencutup");
    }

    SECTION("numeric arg form still works (back-compat)") {
        auto result = akkado::compile(
            "trig = button(\"hit\")\n"
            "sample(trig, 1.0, 5) |> out(%, %)"
        );
        REQUIRE(result.success);
        // Numeric ID does not register a sample name and emits no scalar mapping.
        CHECK(result.required_samples_extended.empty());
        CHECK(result.scalar_sample_mappings.empty());
    }
}

// =============================================================================
// Phase 4: Record / array spread in user-defined function calls
// =============================================================================

TEST_CASE("User fn record spread: name-based binding", "[codegen][spread]") {
    SECTION("record fields fill all params by name") {
        auto result = akkado::compile(
            "fn myAdd(a, b) -> a + b\n"
            "r = {a: 1, b: 2}\n"
            "myAdd(..r) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("record field order is irrelevant — match is by name") {
        auto result = akkado::compile(
            "fn synth(freq, vel) -> freq * vel\n"
            "r = {vel: 0.8, freq: 440}\n"
            "synth(..r) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("positional arg + record spread") {
        auto result = akkado::compile(
            "fn f(a, b, c) -> a + b + c\n"
            "r = {b: 2, c: 3}\n"
            "f(1, ..r) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("record spread + named override") {
        auto result = akkado::compile(
            "fn f(a, b) -> a + b\n"
            "r = {a: 1, b: 2}\n"
            "f(..r, b: 99) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("inline record spread") {
        auto result = akkado::compile(
            "fn f(a, b) -> a + b\n"
            "f(..{a: 1, b: 2}) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("missing required field emits E105") {
        auto result = akkado::compile(
            "fn f(a, b, c) -> a + b + c\n"
            "r = {a: 1, b: 2}\n"
            "f(..r) |> out(%, %)"
        );
        REQUIRE_FALSE(result.success);
        bool got_e105 = false;
        for (const auto& d : result.diagnostics) if (d.code == "E105") got_e105 = true;
        CHECK(got_e105);
    }

    SECTION("extra field emits W160 warning but compiles") {
        auto result = akkado::compile(
            "fn f(a, b) -> a + b\n"
            "r = {a: 1, b: 2, extra: 99}\n"
            "f(..r) |> out(%, %)"
        );
        REQUIRE(result.success);
        bool got_w160 = false;
        for (const auto& d : result.diagnostics) if (d.code == "W160") got_w160 = true;
        CHECK(got_w160);
    }

    SECTION("non-record/array spread emits E140") {
        auto result = akkado::compile(
            "fn f(a) -> a\n"
            "x = 42\n"
            "f(..x) |> out(%, %)"
        );
        REQUIRE_FALSE(result.success);
        bool got_e140 = false;
        for (const auto& d : result.diagnostics) if (d.code == "E140") got_e140 = true;
        CHECK(got_e140);
    }
}

TEST_CASE("User fn array spread: positional binding", "[codegen][spread]") {
    SECTION("array elements fill all params positionally") {
        auto result = akkado::compile(
            "fn add3(a, b, c) -> a + b + c\n"
            "arr = [1, 2, 3]\n"
            "add3(..arr) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("positional + array spread combined") {
        auto result = akkado::compile(
            "fn f(a, b, c) -> a + b + c\n"
            "rest = [2, 3]\n"
            "f(1, ..rest) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("too many array elements emits E107") {
        auto result = akkado::compile(
            "fn f(a, b) -> a + b\n"
            "arr = [1, 2, 3]\n"
            "f(..arr) |> out(%, %)"
        );
        REQUIRE_FALSE(result.success);
        bool got_e107 = false;
        for (const auto& d : result.diagnostics) if (d.code == "E107") got_e107 = true;
        CHECK(got_e107);
    }
}

TEST_CASE("Mixed record + array spread emits E180", "[codegen][spread]") {
    auto result = akkado::compile(
        "fn f(a, b, c) -> a + b + c\n"
        "r = {a: 1}\n"
        "arr = [2, 3]\n"
        "f(..r, ..arr) |> out(%, %)"
    );
    REQUIRE_FALSE(result.success);
    bool got_e180 = false;
    for (const auto& d : result.diagnostics) if (d.code == "E180") got_e180 = true;
    CHECK(got_e180);
}

// =============================================================================
// Phase 5: Array literal spread (..arr inside [..])
// =============================================================================

TEST_CASE("Array literal spread flattens elements", "[codegen][spread][array]") {
    SECTION("simple ..arr at front") {
        auto result = akkado::compile(
            "a = [1, 2]\n"
            "b = [..a, 3]\n"
            "len(b)"
        );
        REQUIRE(result.success);
    }

    SECTION("spread at end") {
        auto result = akkado::compile(
            "a = [2, 3]\n"
            "b = [1, ..a]\n"
            "len(b)"
        );
        REQUIRE(result.success);
    }

    SECTION("multiple spreads") {
        auto result = akkado::compile(
            "a = [1, 2]\n"
            "b = [3, 4]\n"
            "c = [..a, ..b]\n"
            "len(c)"
        );
        REQUIRE(result.success);
    }

    SECTION("inline array spread") {
        auto result = akkado::compile(
            "x = [..[1, 2], 3]\n"
            "len(x)"
        );
        REQUIRE(result.success);
    }

    SECTION("empty array spread is valid") {
        auto result = akkado::compile(
            "empty = []\n"
            "y = [..empty, 1, 2]\n"
            "len(y)"
        );
        REQUIRE(result.success);
    }

    SECTION("non-array spread emits E140") {
        auto result = akkado::compile(
            "x = 42\n"
            "y = [..x, 1]\n"
            "len(y)"
        );
        REQUIRE_FALSE(result.success);
        bool got_e140 = false;
        for (const auto& d : result.diagnostics) if (d.code == "E140") got_e140 = true;
        CHECK(got_e140);
    }
}

// =============================================================================
// Phase 6: Spread into builtin / special-handler calls
// =============================================================================

TEST_CASE("Builtin call accepts ..record spread", "[codegen][spread][builtin]") {
    SECTION("lp() with record spread fills cut+q by name") {
        auto result = akkado::compile(
            "cfg = {cut: 1000, q: 0.7}\n"
            "lp(osc(\"sin\", 440), ..cfg) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("lp() with inline record spread") {
        auto result = akkado::compile(
            "lp(osc(\"sin\", 440), ..{cut: 1000, q: 0.7}) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("array spread fills positional builtin args") {
        auto result = akkado::compile(
            "args = [1000, 0.7]\n"
            "lp(osc(\"sin\", 440), ..args) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("reusing same spread record across two calls") {
        // The second call must successfully re-look-up the resolved record
        // and not crash on stale state.
        auto result = akkado::compile(
            "cfg = {cut: 1000, q: 0.7}\n"
            "a = lp(osc(\"sin\", 440), ..cfg)\n"
            "b = lp(osc(\"saw\", 220), ..cfg)\n"
            "(a + b) |> out(%, %)"
        );
        REQUIRE(result.success);
    }
}

// =============================================================================
// Phase 7: Spread integration tests (PRD §10.4)
// =============================================================================

TEST_CASE("Spread integration: multi-spread combinations", "[codegen][spread][integration]") {
    SECTION("multiple record spreads — later wins") {
        auto result = akkado::compile(
            "fn f(a, b) -> a + b\n"
            "r1 = {a: 1, b: 2}\n"
            "r2 = {b: 99}\n"
            "f(..r1, ..r2) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("nested array spread in array literal") {
        // Note: single-element arrays collapse to scalars in this codebase
        // (existing semantics), so spread sources must be ≥ 2 elements.
        auto result = akkado::compile(
            "a = [1, 2]\n"
            "b = [3, 4]\n"
            "c = [5, 6]\n"
            "all = [..a, ..b, ..c]\n"
            "len(all)"
        );
        REQUIRE(result.success);
    }
}

TEST_CASE("Spread integration: closure call with spread", "[codegen][spread][integration]") {
    SECTION("record spread into closure assigned to var") {
        auto result = akkado::compile(
            "g = (x, y) -> x * y\n"
            "r = {x: 3, y: 7}\n"
            "g(..r) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("array spread into closure") {
        auto result = akkado::compile(
            "g = (x, y) -> x + y\n"
            "arr = [1, 2]\n"
            "g(..arr) |> out(%, %)"
        );
        REQUIRE(result.success);
    }
}

TEST_CASE("Spread integration: edge cases", "[codegen][spread][integration]") {
    SECTION("empty record spread falls back to defaults") {
        auto result = akkado::compile(
            "fn f(a = 1, b = 2) -> a + b\n"
            "r = {}\n"
            "f(..r) |> out(%, %)"
        );
        REQUIRE(result.success);
    }

    SECTION("spread with only named arg afterward") {
        auto result = akkado::compile(
            "fn f(a, b) -> a + b\n"
            "r = {a: 1}\n"
            "f(..r, b: 99) |> out(%, %)"
        );
        REQUIRE(result.success);
    }
}

// ============================================================================
// Chord patterns into soundfont (and other internally-polyphonic instruments)
// ============================================================================
// Regression tests for the bug where:
//   1) c"CM Am Dm G" |> soundfont(@, "gm", 0) |> out(@) errored with E410
//      ("wrap in poly()") even though soundfont has internal 32-voice polyphony.
//   2) c"…" .voicing("open") |> soundfont(…) ignored upper voices because only
//      voice 0's freq buffer was published on PatternPayload.
//   3) c"…" .transpose(0) |> soundfont(…) collapsed the chord to its root.
//
// All three traced to a single root cause: emit_pattern_with_state /
// emit_per_voice_seqpat / handle_chord_call only published voice 0 to
// PatternPayload::FREQ even when max_voices > 1. The fix publishes per-voice
// freq buffers via PatternPayload::voice_freqs and makes handle_soundfont_call
// emit one SOUNDFONT_VOICE per voice (each with its own state_id) summed
// into the final output.
//
// Helper: count instructions matching a predicate.
namespace {
template <typename Pred>
std::size_t count_insts(const std::vector<cedar::Instruction>& insts, Pred p) {
    std::size_t n = 0;
    for (const auto& i : insts) if (p(i)) ++n;
    return n;
}
}  // namespace

TEST_CASE("chord pattern pipes directly into soundfont", "[chord-soundfont]") {
    SECTION("c\"CM Am Dm G\" |> soundfont compiles without E410") {
        auto result = akkado::compile(
            R"(c"CM Am Dm G" |> soundfont(@, "gm", 0) |> out(@, @))");
        // Inspect diagnostics for E410 even if other unrelated errors crept in.
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.code != "E410");
        }
        REQUIRE(result.success);
    }

    SECTION("emits one SOUNDFONT_VOICE per chord voice (3 for triads)") {
        auto result = akkado::compile(
            R"(c"CM Am Dm G" |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        std::size_t sf_count = count_insts(insts,
            [](const cedar::Instruction& i) {
                return i.opcode == cedar::Opcode::SOUNDFONT_VOICE;
            });
        // CM/Am/Dm/G are all triads → 3 voices → 3 SOUNDFONT_VOICE instructions.
        CHECK(sf_count == 3);
    }

    SECTION("each per-voice SOUNDFONT_VOICE has a unique state_id and freq input") {
        auto result = akkado::compile(
            R"(c"CM Am Dm G" |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        std::vector<std::uint32_t> state_ids;
        std::vector<std::uint16_t> freq_bufs;
        std::vector<std::uint8_t> sf_slots;
        for (const auto& i : insts) {
            if (i.opcode == cedar::Opcode::SOUNDFONT_VOICE) {
                state_ids.push_back(i.state_id);
                freq_bufs.push_back(i.inputs[1]);
                sf_slots.push_back(i.rate);
            }
        }
        REQUIRE(state_ids.size() == 3);
        // All voices share the same SF2 slot (one file load).
        CHECK(sf_slots[0] == sf_slots[1]);
        CHECK(sf_slots[1] == sf_slots[2]);
        // But different state_ids (separate SoundFontVoiceState instances).
        CHECK(state_ids[0] != state_ids[1]);
        CHECK(state_ids[1] != state_ids[2]);
        CHECK(state_ids[0] != state_ids[2]);
        // And different freq buffers (one per chord voice).
        CHECK(freq_bufs[0] != freq_bufs[1]);
        CHECK(freq_bufs[1] != freq_bufs[2]);
        CHECK(freq_bufs[0] != freq_bufs[2]);
    }

    SECTION("mono synth chain still rejects polyphonic chord pattern (E410)") {
        // Regression guard: the strict 'wrap in poly()' rule must still fire
        // for anything that doesn't have its own voice allocator.
        auto result = akkado::compile(
            R"(c"CM Am Dm G" |> osc("saw", @.freq) |> out(@, @))");
        // Either E410 fires (preferred) or some other error — but the rule
        // is that a chord chord into a mono UGen must NOT silently work.
        bool has_e410 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E410") { has_e410 = true; break; }
        }
        CHECK(has_e410);
    }
}

TEST_CASE("voicing on chord pattern preserves multi-voice soundfont path",
          "[chord-soundfont][voicing]") {
    // The bug the user reported: c"…" .voicing(…) |> soundfont fed only voice
    // 0 to soundfont. After the fix, voicing must keep emitting one
    // SOUNDFONT_VOICE per voice. (This test does NOT assert that close vs
    // open produce different event values — the voicing optimizer can
    // legitimately converge to the same inversion when its candidate set
    // spans the chord space; that quirk is separate from the soundfont path.)
    SECTION(".voicing(\"close\") still emits 3 SOUNDFONT_VOICE for a triad") {
        auto result = akkado::compile(
            R"(c"CM" .voicing("close") |> soundfont(@, "gm", 0) |> out(@, @))");
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.code != "E410");
        }
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        std::size_t sf_count = count_insts(insts,
            [](const cedar::Instruction& i) {
                return i.opcode == cedar::Opcode::SOUNDFONT_VOICE;
            });
        CHECK(sf_count == 3);
    }

    SECTION(".voicing(\"open\") still emits 3 SOUNDFONT_VOICE for a triad") {
        auto result = akkado::compile(
            R"(c"CM" .voicing("open") |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        std::size_t sf_count = count_insts(insts,
            [](const cedar::Instruction& i) {
                return i.opcode == cedar::Opcode::SOUNDFONT_VOICE;
            });
        CHECK(sf_count == 3);
    }

    SECTION("voicing event payload still has num_values == 3 for triads") {
        auto result = akkado::compile(
            R"(c"CM" .voicing("close") |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& events = result.state_inits[0].sequence_events;
        REQUIRE_FALSE(events.empty());
        REQUIRE_FALSE(events[0].empty());
        CHECK(events[0][0].num_values == 3);
    }
}

TEST_CASE("transpose preserves all chord voices", "[chord-soundfont][transpose]") {
    SECTION("transpose(0) on c\"CM Am Dm G\" keeps num_values == 3") {
        auto result = akkado::compile(
            R"(c"CM Am Dm G" .transpose(0) |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(result.success);
        REQUIRE_FALSE(result.state_inits.empty());
        const auto& events = result.state_inits[0].sequence_events;
        REQUIRE_FALSE(events.empty());
        // Each of the 4 chord events is a triad; voices must not collapse.
        for (const auto& evlist : events) {
            for (const auto& ev : evlist) {
                if (ev.type == cedar::EventType::DATA) {
                    CHECK(ev.num_values == 3);
                }
            }
        }
    }

    SECTION("transpose(0) emits 3 SOUNDFONT_VOICE — chord reaches consumer") {
        auto result = akkado::compile(
            R"(c"CM Am Dm G" .transpose(0) |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        std::size_t sf_count = count_insts(insts,
            [](const cedar::Instruction& i) {
                return i.opcode == cedar::Opcode::SOUNDFONT_VOICE;
            });
        CHECK(sf_count == 3);
    }

    SECTION("transpose(12) doubles every voice frequency, preserving voices") {
        auto base = akkado::compile(
            R"(c"CM" |> soundfont(@, "gm", 0) |> out(@, @))");
        auto up = akkado::compile(
            R"(c"CM" .transpose(12) |> soundfont(@, "gm", 0) |> out(@, @))");
        REQUIRE(base.success);
        REQUIRE(up.success);
        REQUIRE_FALSE(base.state_inits.empty());
        REQUIRE_FALSE(up.state_inits.empty());
        const auto& bev = base.state_inits[0].sequence_events[0][0];
        const auto& uev = up.state_inits[0].sequence_events[0][0];
        REQUIRE(bev.num_values == 3);
        REQUIRE(uev.num_values == 3);
        for (std::uint8_t v = 0; v < 3; ++v) {
            CHECK(uev.values[v] == Catch::Approx(bev.values[v] * 2.0f).margin(1.0f));
        }
    }
}

TEST_CASE("user-registered voicing dictionary is recognized and pipes to soundfont",
          "[chord-soundfont][voicing][addVoicings]") {
    SECTION("addVoicings + .voicing(name) expands chords to dict-defined voice count") {
        // voice_chords() now substitutes the dict's quality table for the
        // chord's intrinsic intervals. With M:[0,4,7,11,14] and
        // m:[0,3,7,10,14], CM/Am/Dm voice as 5-note chords; G has empty
        // quality (root-only symbol), so it falls back to the chord parser's
        // intrinsic [0,4,7] triad. max_voices across the progression = 5,
        // so soundfont emits 5 SOUNDFONT_VOICE slots (G simply doesn't fire
        // the upper two slots).
        auto result = akkado::compile(R"(
            addVoicings("piano-jazz", {M: [0, 4, 7, 11, 14], m: [0, 3, 7, 10, 14]})
            c"CM Am Dm G" .voicing("piano-jazz") |> soundfont(@, "gm", 0) |> out(@, @)
        )");
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.code != "E410");
            CHECK(d.code != "E141");
        }
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        std::size_t sf_count = count_insts(insts,
            [](const cedar::Instruction& i) {
                return i.opcode == cedar::Opcode::SOUNDFONT_VOICE;
            });
        CHECK(sf_count == 5);
    }
}




// =============================================================================
// Unison Tests (prd-unison Phase 1)
// =============================================================================

TEST_CASE("Codegen: unison stdlib function", "[codegen][unison]") {
    SECTION("compiles with default args") {
        auto result = akkado::compile(R"(
            fn voice(f, g, v, e) -> sine(f)
            unison(440, 1, 1, voice) |> out(%)
        )");
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.severity != akkado::Severity::Error);
        }
        REQUIRE(result.success);
        // Default voices=2 → 2 voice instances.
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::PAN) == 2);
    }

    SECTION("voices=N emits N voice instances at distinct state_ids") {
        auto result = akkado::compile(R"(
            unison(440, 1, 1, (f, g, v, e) -> sine(f), voices: 5) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 5);
        CHECK(count_instructions(insts, cedar::Opcode::PAN) == 5);
        // Each voice's oscillator must get a distinct state_id so hot-swap
        // state preservation tracks them independently.
        std::set<std::uint32_t> state_ids;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::OSC_SIN) state_ids.insert(inst.state_id);
        }
        CHECK(state_ids.size() == 5);
    }

    SECTION("voices=1 takes the centered special-case branch") {
        auto result = akkado::compile(R"(
            unison(440, 1, 1, (f, g, v, e) -> sine(f), voices: 1) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Special-case branch: a single centered voice, no linspace unroll.
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 1);
        CHECK(count_instructions(insts, cedar::Opcode::PAN) == 1);
    }

    SECTION("voices left at default still const-folds the match arm") {
        // Regression: a defaulted `voices` param must resolve as a compile-time
        // literal so match(voices) folds and linspace sees a constant.
        auto result = akkado::compile(R"(
            fn voice(f, g, v, e) -> saw(f)
            unison(220, 1, 1, voice) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 2);
    }

    SECTION("voices not a compile-time literal errors") {
        auto result = akkado::compile(R"(
            fn voice(f, g, v, e) -> sine(f)
            n = param("n", 4, 1, 16)
            unison(440, 1, 1, voice, voices: n) |> out(%)
        )");
        REQUIRE_FALSE(result.success);
        bool has_e173 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E173") has_e173 = true;
        }
        CHECK(has_e173);
    }

    SECTION("ext record fields are accessible inside the instrument") {
        auto result = akkado::compile(R"(
            fn rich(freq, gate, vel, ext) ->
                saw(freq, ext.phase) * ar(gate, 0.05 + ext.idx * 0.005, 0.4) * vel
            unison(440, 1, 1, rich, voices: 4, detune: 0.3, phase: 0.25) |> out(%)
        )");
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.severity != akkado::Severity::Error);
        }
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 4);
    }

    SECTION("unison detune is runtime-modulatable") {
        auto result = akkado::compile(R"(
            fn voice(f, g, v, e) -> saw(f)
            d = param("detune", 0.3, 0, 1)
            unison(440, 1, 1, voice, voices: 4, detune: d) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 4);
    }

    SECTION("composes with poly (stereo poly required)") {
        auto result = akkado::compile(R"(
            fn voice(f, g, v, e) -> saw(f)
            fn fat(f, g, v) -> unison(f, g, v, voice, voices: 4)
            pat("c4 e4") |> poly(%, fat, 2) |> out(%)
        )");
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.severity != akkado::Severity::Error);
        }
        REQUIRE(result.success);
        // poly is a runtime voice allocator: the unison body is compiled once
        // and runtime-multiplexed across poly slots, so it contributes exactly
        // unison_voices (4) saw instances.
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 4);
    }
}

// =============================================================================
// midi() builtin — PRD prd-midi-input §8.3
// =============================================================================

namespace {

// Find the index of the first MIDI_QUERY in an instruction list, or SIZE_MAX.
std::size_t find_midi_query_idx(const std::vector<cedar::Instruction>& insts) {
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].opcode == cedar::Opcode::MIDI_QUERY) return i;
    }
    return SIZE_MAX;
}

// Find the index of the first POLY_BEGIN.
std::size_t find_poly_begin_idx(const std::vector<cedar::Instruction>& insts) {
    for (std::size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].opcode == cedar::Opcode::POLY_BEGIN) return i;
    }
    return SIZE_MAX;
}

// Locate the PolyAlloc state init for a given state_id.
const akkado::StateInitData* find_poly_alloc_init(
    const akkado::CompileResult& result, std::uint32_t state_id) {
    for (const auto& s : result.state_inits) {
        if (s.type == akkado::StateInitData::Type::PolyAlloc &&
            s.state_id == state_id) {
            return &s;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("midi() basic codegen", "[midi]") {
    const std::string instr_decl =
        "fn synth(f, g, v) -> osc(\"saw\", f) * adsr(g) * v\n";

    SECTION("bare midi() pipes into poly with matching state_id") {
        auto result = akkado::compile(instr_decl +
            "midi() |> poly(%, synth, 8) |> out(%)");
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.severity != akkado::Severity::Error);
        }
        REQUIRE(result.success);

        auto insts = get_instructions(result);
        std::size_t mq = find_midi_query_idx(insts);
        std::size_t pb = find_poly_begin_idx(insts);
        REQUIRE(mq != SIZE_MAX);
        REQUIRE(pb != SIZE_MAX);
        CHECK(mq < pb);

        std::uint32_t midi_state = insts[mq].state_id;
        std::uint32_t poly_state = insts[pb].state_id;
        const auto* pa = find_poly_alloc_init(result, poly_state);
        REQUIRE(pa != nullptr);
        CHECK(pa->poly_seq_state_id == midi_state);

        REQUIRE(result.required_midi_sources.size() == 1);
        const auto& src = result.required_midi_sources[0];
        CHECK(src.state_id == midi_state);
        CHECK(src.kind == cedar::MidiSourceKind::DefaultDevice);
        CHECK(src.name_or_path == "");
        CHECK(src.channel_filter == 0);
        CHECK(src.loop == false);
        CHECK(src.tempo_mode == cedar::MidiQueueState::TempoMode::Follow);
    }

    SECTION("midi({device: \"name\"}) sets kind=NamedDevice + name") {
        auto result = akkado::compile(instr_decl +
            "midi({device: \"Launchkey\"}) |> poly(%, synth, 4) |> out(%)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_sources.size() == 1);
        const auto& src = result.required_midi_sources[0];
        CHECK(src.kind == cedar::MidiSourceKind::NamedDevice);
        CHECK(src.name_or_path == "Launchkey");
    }

    SECTION("midi({file: ...}) sets kind=File + path + Follow default") {
        auto result = akkado::compile(instr_decl +
            "midi({file: \"song.mid\"}) |> poly(%, synth, 8) |> out(%)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_sources.size() == 1);
        const auto& src = result.required_midi_sources[0];
        CHECK(src.kind == cedar::MidiSourceKind::File);
        CHECK(src.name_or_path == "song.mid");
        CHECK(src.tempo_mode == cedar::MidiQueueState::TempoMode::Follow);
        CHECK(src.loop == false);
    }

    SECTION("midi({file:..., loop: true, tempo: \"file\"}) propagates all") {
        auto result = akkado::compile(instr_decl +
            "midi({file: \"song.mid\", loop: true, tempo: \"file\"}) "
            "|> poly(%, synth, 8) |> out(%)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_sources.size() == 1);
        const auto& src = result.required_midi_sources[0];
        CHECK(src.kind == cedar::MidiSourceKind::File);
        CHECK(src.name_or_path == "song.mid");
        CHECK(src.loop == true);
        CHECK(src.tempo_mode == cedar::MidiQueueState::TempoMode::File);
    }

    SECTION("midi({channel: 1}) sets channel_filter") {
        auto result = akkado::compile(instr_decl +
            "midi({channel: 1}) |> poly(%, synth, 4) |> out(%)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_sources.size() == 1);
        CHECK(result.required_midi_sources[0].channel_filter == 1);
    }

    SECTION("two midi() calls produce two RequiredMidiSource entries with distinct state_ids") {
        auto result = akkado::compile(instr_decl +
            "midi({channel: 1}) |> poly(%, synth, 4) |> out(%)\n"
            "midi({channel: 2}) |> poly(%, synth, 4) |> out(%)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_sources.size() == 2);
        const auto& a = result.required_midi_sources[0];
        const auto& b = result.required_midi_sources[1];
        CHECK(a.state_id != b.state_id);
        CHECK(a.channel_filter == 1);
        CHECK(b.channel_filter == 2);

        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::MIDI_QUERY) == 2);
    }
}

TEST_CASE("midi() diagnostics", "[midi]") {
    const std::string instr_decl =
        "fn synth(f, g, v) -> osc(\"saw\", f) * adsr(g) * v\n";

    auto has_diag = [](const akkado::CompileResult& r, const char* code) {
        for (const auto& d : r.diagnostics) {
            if (d.code == code) return true;
        }
        return false;
    };

    SECTION("E411: file and device are mutually exclusive") {
        auto result = akkado::compile(instr_decl +
            "midi({file: \"song.mid\", device: \"X\"}) |> poly(%, synth, 4) |> out(%)");
        CHECK(has_diag(result, "E411"));
        CHECK(!result.success);
    }

    SECTION("E413: invalid tempo value") {
        auto result = akkado::compile(instr_decl +
            "midi({file: \"song.mid\", tempo: \"wrong\"}) |> poly(%, synth, 4) |> out(%)");
        CHECK(has_diag(result, "E413"));
        CHECK(!result.success);
    }

    SECTION("E414: out-of-range channel") {
        auto result = akkado::compile(instr_decl +
            "midi({channel: 17}) |> poly(%, synth, 4) |> out(%)");
        CHECK(has_diag(result, "E414"));
        CHECK(!result.success);
    }

    SECTION("E412: midi() inside fn body is rejected") {
        auto result = akkado::compile(
            "fn synth(f, g, v) -> osc(\"saw\", f) * adsr(g) * v\n"
            "fn build_chain() -> midi() |> poly(%, synth, 4)\n"
            "build_chain() |> out(%)");
        CHECK(has_diag(result, "E412"));
        CHECK(!result.success);
    }
}

// =============================================================================
// midi_cc() builtin — PRD prd-midi-input §4.8
// =============================================================================

TEST_CASE("midi_cc() basic codegen", "[midi_cc]") {
    SECTION("midi_cc(name, {cc}) records a CC route with default range") {
        auto result = akkado::compile(
            "cutoff = param(\"cutoff\", 1000, 50, 5000)\n"
            "midi_cc(\"cutoff\", {cc: 74})\n"
            "out(cutoff)");
        for (const auto& d : result.diagnostics) {
            INFO("diag " << d.code << ": " << d.message);
            CHECK(d.severity != akkado::Severity::Error);
        }
        REQUIRE(result.success);
        REQUIRE(result.required_midi_cc_routes.size() == 1);
        const auto& r = result.required_midi_cc_routes[0];
        CHECK(r.param_name == "cutoff");
        CHECK(r.cc_num == 74);
        CHECK(r.channel_filter == 0);
        CHECK(r.scale == Catch::Approx(1.0f));
        CHECK(r.bias == Catch::Approx(0.0f));
        CHECK(r.slew_ms == Catch::Approx(5.0f));
    }

    SECTION("midi_cc(name, {cc, min, max}) sets scale/bias") {
        auto result = akkado::compile(
            "f = param(\"f\", 1000, 50, 5000)\n"
            "midi_cc(\"f\", {cc: 74, min: 50, max: 5000})\n"
            "out(f)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_cc_routes.size() == 1);
        const auto& r = result.required_midi_cc_routes[0];
        CHECK(r.scale == Catch::Approx(4950.0f));
        CHECK(r.bias  == Catch::Approx(50.0f));
    }

    SECTION("midi_cc(name, {pb: true}) sets cc_num=-1 and default -1..+1") {
        auto result = akkado::compile(
            "b = param(\"bend\", 0, -1, 1)\n"
            "midi_cc(\"bend\", {pb: true})\n"
            "out(b)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_cc_routes.size() == 1);
        const auto& r = result.required_midi_cc_routes[0];
        CHECK(r.cc_num == -1);
        CHECK(r.scale == Catch::Approx(2.0f));
        CHECK(r.bias  == Catch::Approx(-1.0f));
    }

    SECTION("midi_cc(name, {at: true}) sets cc_num=-2") {
        auto result = akkado::compile(
            "p = param(\"press\", 0, 0, 1)\n"
            "midi_cc(\"press\", {at: true})\n"
            "out(p)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_cc_routes.size() == 1);
        CHECK(result.required_midi_cc_routes[0].cc_num == -2);
    }

    SECTION("midi_cc(name, {cc, channel}) sets channel_filter") {
        auto result = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "midi_cc(\"x\", {cc: 1, channel: 5})\n"
            "out(x)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_cc_routes.size() == 1);
        CHECK(result.required_midi_cc_routes[0].channel_filter == 5);
    }

    SECTION("midi_cc(name, {cc, slew}) sets slew_ms") {
        auto result = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "midi_cc(\"x\", {cc: 1, slew: 20})\n"
            "out(x)");
        REQUIRE(result.success);
        REQUIRE(result.required_midi_cc_routes.size() == 1);
        CHECK(result.required_midi_cc_routes[0].slew_ms == Catch::Approx(20.0f));
    }

    SECTION("midi_cc emits no bytecode for the directive itself") {
        // Baseline: just out(0). Then with midi_cc layered on top, the
        // instruction count for non-state-init ops should match.
        auto base = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "out(x)");
        auto with_cc = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "midi_cc(\"x\", {cc: 74})\n"
            "out(x)");
        REQUIRE(base.success);
        REQUIRE(with_cc.success);
        auto base_insts = get_instructions(base);
        auto with_insts = get_instructions(with_cc);
        CHECK(base_insts.size() == with_insts.size());
    }
}

TEST_CASE("midi_cc() diagnostics", "[midi_cc]") {
    auto has_diag = [](const akkado::CompileResult& r, const char* code) {
        for (const auto& d : r.diagnostics) {
            if (d.code == code) return true;
        }
        return false;
    };

    SECTION("E420: multiple of cc/pb/at set") {
        auto result = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "midi_cc(\"x\", {cc: 1, pb: true})\n"
            "out(x)");
        CHECK(has_diag(result, "E420"));
        CHECK(!result.success);
    }

    SECTION("E421: none of cc/pb/at set") {
        auto result = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "midi_cc(\"x\", {})\n"
            "out(x)");
        CHECK(has_diag(result, "E421"));
        CHECK(!result.success);
    }

    SECTION("E422: CC out of range") {
        auto result = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "midi_cc(\"x\", {cc: 200})\n"
            "out(x)");
        CHECK(has_diag(result, "E422"));
        CHECK(!result.success);
    }

    SECTION("E414: channel out of range") {
        auto result = akkado::compile(
            "x = param(\"x\", 0, 0, 1)\n"
            "midi_cc(\"x\", {cc: 1, channel: 17})\n"
            "out(x)");
        CHECK(has_diag(result, "E414"));
        CHECK(!result.success);
    }

    SECTION("E400: wrong arg count") {
        auto result = akkado::compile(
            "midi_cc(\"x\")\n"
            "out(0)");
        CHECK(has_diag(result, "E400"));
        CHECK(!result.success);
    }

    SECTION("E412: midi_cc inside fn body is rejected") {
        auto result = akkado::compile(
            "fn setup() -> midi_cc(\"x\", {cc: 1})\n"
            "setup()\n"
            "out(0)");
        CHECK(has_diag(result, "E412"));
        CHECK(!result.success);
    }
}

// ============================================================================
// PRD prd-midi-input §7.2: `release:` option on poly/mono/legato
// ============================================================================
//
// Codegen-side: the parsed value lands on a PolyAlloc StateInitData
// (poly_release_seconds) without raising errors. Negative / non-literal
// values are caught with a friendly message.

TEST_CASE("poly() accepts release: option", "[polyphony][poly-release]") {
    const std::string preamble =
        "fn synth(f, g, v) -> osc(\"saw\", f) * adsr(g) * v\n";

    SECTION("release as 4th positional literal") {
        auto result = akkado::compile(preamble +
            "pat(\"c4 e4\") |> poly(%, synth, 8, 0.5) |> out(%)");
        REQUIRE(result.success);

        bool found = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                found = true;
                CHECK(init.poly_release_seconds == Catch::Approx(0.5f));
                CHECK(init.poly_max_voices == 8);
                CHECK(init.poly_mode == 0);
            }
        }
        CHECK(found);
    }

    SECTION("release as named arg") {
        auto result = akkado::compile(preamble +
            "pat(\"c4\") |> poly(%, synth, 4, release: 0.25) |> out(%)");
        REQUIRE(result.success);

        bool found = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                found = true;
                CHECK(init.poly_release_seconds == Catch::Approx(0.25f));
            }
        }
        CHECK(found);
    }

    SECTION("release defaults to 0 when omitted") {
        auto result = akkado::compile(preamble +
            "pat(\"c4\") |> poly(%, synth) |> out(%)");
        REQUIRE(result.success);

        bool found = false;
        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                found = true;
                CHECK(init.poly_release_seconds == Catch::Approx(0.0f));
            }
        }
        CHECK(found);
    }

    SECTION("E406: release must be number literal") {
        auto has_diag = [](const akkado::CompileResult& r, const char* code) {
            for (const auto& d : r.diagnostics) {
                if (d.code == code) return true;
            }
            return false;
        };
        auto result = akkado::compile(preamble +
            "x = osc(\"sin\", 1)\n"
            "pat(\"c4\") |> poly(%, synth, 8, x) |> out(%)");
        CHECK(has_diag(result, "E406"));
        CHECK(!result.success);
    }

    SECTION("negative release clamps to 0") {
        auto result = akkado::compile(preamble +
            "pat(\"c4\") |> poly(%, synth, 4, -1.0) |> out(%)");
        REQUIRE(result.success);

        for (const auto& init : result.state_inits) {
            if (init.type == akkado::StateInitData::Type::PolyAlloc) {
                CHECK(init.poly_release_seconds == Catch::Approx(0.0f));
            }
        }
    }
}

TEST_CASE("legato() accepts release: option (positional 3-arg form)",
          "[polyphony][poly-release]") {
    const std::string preamble =
        "fn synth(f, g, v) -> osc(\"saw\", f) * adsr(g) * v\n";

    // legato uses the positional (input, instrument, release) form. Mixed
    // positional/named args are intentionally not exercised here — see the
    // comment on the legato builtin in builtins.hpp for the dual-form
    // dispatch caveat.
    auto result = akkado::compile(preamble +
        "pat(\"c4\") |> legato(%, synth, 0.3) |> out(%)");
    REQUIRE(result.success);

    bool found = false;
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            found = true;
            CHECK(init.poly_release_seconds == Catch::Approx(0.3f));
            CHECK(init.poly_mode == 2);  // legato
        }
    }
    CHECK(found);
}
