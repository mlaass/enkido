#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <cstring>
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
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }
}

// NOTE: fold() tests skipped - 'fold' name conflicts with wavefolding builtin
// Consider renaming higher-order fold to 'reduce' in future

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

    SECTION("polyphonic oscillator inline") {
        // NOTE: Variable assignment doesn't fully propagate multi-buffers currently
        // Testing inline version without variable
        auto result = akkado::compile("sum(map(mtof(chord(\"Am\")), (f) -> saw(f)))");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Should have multiple SAW oscillators and ADDs to sum them
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) >= 3);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) >= 2);
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
        auto result = akkado::compile(R"(
            mode = dropdown("mode", "default")
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
