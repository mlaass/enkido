#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <cmath>
#include <vector>

// Helper to decode float from PUSH_CONST instruction
static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

static std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    size_t count = result.program.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.program.bytecode.data(), result.program.bytecode.size());
    return instructions;
}

static size_t count_instructions(const std::vector<cedar::Instruction>& insts,
                                  cedar::Opcode op) {
    size_t count = 0;
    for (const auto& inst : insts) {
        if (inst.opcode == op) ++count;
    }
    return count;
}

// =============================================================================
// const variable declarations
// =============================================================================

TEST_CASE("Const: scalar variable", "[const]") {
    auto result = akkado::compile("const x = 42");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    REQUIRE(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
    CHECK(decode_const_float(insts[0]) == 42.0f);
}

TEST_CASE("Const: arithmetic expression", "[const]") {
    auto result = akkado::compile("const y = 2 * 220 + 5");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Should emit a single PUSH_CONST with value 445
    REQUIRE(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
    CHECK(decode_const_float(insts[0]) == 445.0f);
}

TEST_CASE("Const: power right-associativity (F7)", "[const][F7]") {
    // F7 was withdrawn in Phase 0 (commit b203e2e) — `^` is already
    // right-associative on master. These tests lock the numeric value
    // through the full const-eval pipeline. See PRD §1.3, §4 Phase 2.

    SECTION("2 ^ 3 ^ 2 == 512") {
        auto result = akkado::compile("const x = 2 ^ 3 ^ 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
        CHECK(decode_const_float(insts[0]) == 512.0f);
    }

    SECTION("2 ^ 2 ^ 2 ^ 2 == 65536") {
        auto result = akkado::compile("const x = 2 ^ 2 ^ 2 ^ 2");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        REQUIRE(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 1);
        CHECK(decode_const_float(insts[0]) == 65536.0f);
    }
}

TEST_CASE("Const: use const variable in expression", "[const]") {
    auto result = akkado::compile(R"(
        const base = 440
        sine(base)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // base should be a PUSH_CONST(440)
    CHECK(decode_const_float(insts[0]) == 440.0f);
}

TEST_CASE("Const: nested const references", "[const]") {
    auto result = akkado::compile(R"(
        const a = 100
        const b = a * 2
        const c = a + b
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // a = 100, b = 200, c = 300
    CHECK(decode_const_float(insts[0]) == 100.0f);
    CHECK(decode_const_float(insts[1]) == 200.0f);
    CHECK(decode_const_float(insts[2]) == 300.0f);
}

// =============================================================================
// const fn declarations
// =============================================================================

TEST_CASE("Const fn: simple function", "[const]") {
    auto result = akkado::compile(R"(
        const fn double_it(x) -> x * 2
        sine(double_it(220))
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // double_it(220) should be evaluated at compile time to 440
    // Find the PUSH_CONST instructions
    bool found_440 = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            if (decode_const_float(inst) == 440.0f) {
                found_440 = true;
            }
        }
    }
    CHECK(found_440);
}

TEST_CASE("Const fn: mtof conversion", "[const]") {
    auto result = akkado::compile(R"(
        const fn mtof(n) -> 440 * pow(2, (n - 69) / 12)
        sine(mtof(69))
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // mtof(69) = 440 * pow(2, 0) = 440
    bool found_440 = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            if (decode_const_float(inst) == Catch::Approx(440.0f).margin(0.01f)) {
                found_440 = true;
            }
        }
    }
    CHECK(found_440);
}

TEST_CASE("Const fn: with const variable argument", "[const]") {
    auto result = akkado::compile(R"(
        const a = 60
        const fn mtof(n) -> 440 * pow(2, (n - 69) / 12)
        const freq = mtof(a)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // mtof(60) ≈ 261.626
    bool found_freq = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            float val = decode_const_float(inst);
            if (std::abs(val - 261.626f) < 0.1f) {
                found_freq = true;
            }
        }
    }
    CHECK(found_freq);
}

TEST_CASE("Const fn: with default parameter", "[const]") {
    auto result = akkado::compile(R"(
        const fn scale(x, factor = 2) -> x * factor
        sine(scale(220))
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // scale(220) = 220 * 2 = 440
    bool found_440 = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            if (decode_const_float(inst) == 440.0f) {
                found_440 = true;
            }
        }
    }
    CHECK(found_440);
}

// =============================================================================
// const with array generation functions
// =============================================================================

TEST_CASE("Const: linspace accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const count = 4
        linspace(0, 1, count)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Should produce 4 PUSH_CONST values: 0.0, 0.333, 0.667, 1.0
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 5);  // count + linspace values
}

TEST_CASE("Const: range accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const start = 0
        const end = 4
        range(start, end)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // range(0, 4) = [0, 1, 2, 3] -> 4 values plus the const definitions
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 6);  // 2 consts + 4 range values
}

TEST_CASE("Const: harmonics accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const fund = 100
        const count = 3
        harmonics(fund, count)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // harmonics(100, 3) = [100, 200, 300]
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 5);
}

TEST_CASE("Const: random accepts const variable", "[const]") {
    auto result = akkado::compile(R"(
        const count = 3
        random(count)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // random(3) = 3 random values + 1 const
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 4);
}

// =============================================================================
// Purity errors
// =============================================================================

TEST_CASE("Const fn: rejects non-pure operations", "[const]") {
    auto result = akkado::compile(R"(
        const fn bad(x) -> sine(x)
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const fn: rejects non-const function calls", "[const]") {
    auto result = akkado::compile(R"(
        fn regular(x) -> x * 2
        const fn uses_regular(x) -> regular(x)
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const: rejects non-pure rhs", "[const]") {
    auto result = akkado::compile(R"(
        const x = param("freq", 440)
    )");
    CHECK_FALSE(result.success);
}

// =============================================================================
// Const fn: math builtins
// =============================================================================

TEST_CASE("Const fn: uses math builtins", "[const]") {
    auto result = akkado::compile(R"(
        const fn db_to_amp(db) -> pow(10, db / 20)
        sine(440) |> % * db_to_amp(-6)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // db_to_amp(-6) ≈ 0.501
    bool found = false;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            float val = decode_const_float(inst);
            if (std::abs(val - 0.501f) < 0.01f) {
                found = true;
            }
        }
    }
    CHECK(found);
}

TEST_CASE("Const: division by zero error", "[const]") {
    auto result = akkado::compile("const x = 1 / 0");
    CHECK_FALSE(result.success);
}

// Helper to check if a diagnostic with a specific code exists
static bool has_error_code(const std::vector<akkado::Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& d : diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// Helper to find a PUSH_CONST with approximately the given value
static bool has_const_approx(const std::vector<cedar::Instruction>& insts, float target, float margin = 0.01f) {
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            if (std::abs(decode_const_float(inst) - target) < margin) {
                return true;
            }
        }
    }
    return false;
}

// =============================================================================
// Group 1: Comparison and Boolean Operators (Bug 1)
// Operators like >, <, ==, != desugar to gt(), lt(), eq(), neq() calls.
// These must be in the purity whitelist AND implemented in const_eval.
// =============================================================================

TEST_CASE("Const: comparison in const fn (>)", "[const][bug1]") {
    // a > b desugars to gt(a, b) which must be in purity whitelist
    auto result = akkado::compile(R"(
        const fn max2(a, b) -> select(a > b, a, b)
        const v = max2(3, 7)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 7.0f));
}

TEST_CASE("Const: equality check (==)", "[const][bug1]") {
    auto result = akkado::compile(R"(
        const fn eq_check(x) -> select(x == 0, 1, 0)
        const v = eq_check(0)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 1.0f));
}

TEST_CASE("Const: inequality check (!=)", "[const][bug1]") {
    auto result = akkado::compile(R"(
        const fn neq_check(x) -> select(x != 0, 1, 0)
        const v = neq_check(5)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 1.0f));
}

TEST_CASE("Const: less than (<)", "[const][bug1]") {
    auto result = akkado::compile(R"(
        const fn is_neg(x) -> select(x < 0, 1, 0)
        const v = is_neg(-5)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 1.0f));
}

TEST_CASE("Const: less-equal and greater-equal", "[const][bug1]") {
    auto result = akkado::compile(R"(
        const fn in_range(x, lo, hi) -> select(x >= lo, select(x <= hi, 1, 0), 0)
        const v = in_range(5, 0, 10)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 1.0f));
}

TEST_CASE("Const: boolean AND (&&)", "[const][bug1]") {
    // a && b desugars to band(a, b)
    auto result = akkado::compile(R"(
        const v = select(1 > 0 && 2 > 1, 10, 20)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 10.0f));
}

TEST_CASE("Const: boolean OR (||)", "[const][bug1]") {
    // a || b desugars to bor(a, b)
    auto result = akkado::compile(R"(
        const v = select(0 > 1 || 2 > 1, 10, 20)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 10.0f));
}

TEST_CASE("Const: boolean NOT (!)", "[const][bug1]") {
    // !a desugars to bnot(a)
    auto result = akkado::compile(R"(
        const v = select(!0, 10, 20)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 10.0f));
}

TEST_CASE("Const: complex condition with comparisons", "[const][bug1]") {
    auto result = akkado::compile(R"(
        const fn clamp2(x, lo, hi) -> select(x < lo, lo, select(x > hi, hi, x))
        const v = clamp2(5, 0, 10)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 5.0f));
}

TEST_CASE("Const: comparison in const variable", "[const][bug1]") {
    // Even simple const expressions with operators should work
    auto result = akkado::compile("const v = select(3 > 2, 100, 200)");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 100.0f));
}

// =============================================================================
// Group 2: Forward References and Ordering (Bug 2)
// =============================================================================

TEST_CASE("Const: forward reference gives error", "[const][bug2]") {
    auto result = akkado::compile(R"(
        const a = b * 2
        const b = 50
    )");
    // Forward references should fail — b is not yet initialized when a is evaluated
    CHECK_FALSE(result.success);
}

TEST_CASE("Const: sequential references work", "[const]") {
    auto result = akkado::compile(R"(
        const a = 10
        const b = a * 2
        const c = b + a
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 10.0f));
    CHECK(has_const_approx(insts, 20.0f));
    CHECK(has_const_approx(insts, 30.0f));
}

// =============================================================================
// Group 3: Const Fn Interactions
// =============================================================================

TEST_CASE("Const fn: calling another const fn", "[const]") {
    auto result = akkado::compile(R"(
        const fn sq(x) -> x * x
        const fn sum_sq(a, b) -> sq(a) + sq(b)
        const v = sum_sq(3, 4)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 25.0f));
}

TEST_CASE("Const fn: zero arguments", "[const]") {
    auto result = akkado::compile(R"(
        const fn pi() -> acos(-1)
        const p = pi()
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 3.14159f, 0.001f));
}

TEST_CASE("Const fn: all default arguments", "[const]") {
    auto result = akkado::compile(R"(
        const fn freq(oct = 4) -> 440 * pow(2, oct - 4)
        const f = freq()
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 440.0f));
}

TEST_CASE("Const fn: param shadows outer const", "[const]") {
    auto result = akkado::compile(R"(
        const x = 100
        const fn use_x(x) -> x * 2
        const v = use_x(5)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 10.0f));
}

TEST_CASE("Const fn: param name same as builtin", "[const]") {
    auto result = akkado::compile(R"(
        const fn test_fn(sin) -> sin * 2
        const v = test_fn(5)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 10.0f));
}

TEST_CASE("Const fn: block body with let", "[const]") {
    auto result = akkado::compile(R"(
        const fn complex(x) -> {
            y = x * 2
            z = y + 1
            z * z
        }
        const v = complex(3)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 49.0f));
}

TEST_CASE("Const fn: chained const fn calls", "[const]") {
    // Test multiple const fns calling each other
    auto result = akkado::compile(R"(
        const fn square(x) -> x * x
        const fn sum_of_squares(a, b) -> square(a) + square(b)
        const fn hyp(a, b) -> sqrt(sum_of_squares(a, b))
        const v = hyp(3, 4)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // sqrt(9 + 16) = sqrt(25) = 5
    CHECK(has_const_approx(insts, 5.0f));
}

TEST_CASE("Const fn: recursion depth limit", "[const]") {
    auto result = akkado::compile(R"(
        const fn inf(x) -> inf(x + 1)
        const v = inf(0)
    )");
    CHECK_FALSE(result.success);
}

// =============================================================================
// Group 4: Match Expressions in Const
// =============================================================================

TEST_CASE("Const fn: match with scrutinee", "[const]") {
    // match uses match(expr) { } syntax, not match expr { }
    auto result = akkado::compile(R"(
        const fn classify(n) -> match(n) { 0: 10, 1: 20, _: 30 }
        const v = classify(1)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 20.0f));
}

TEST_CASE("Const fn: match with range pattern", "[const]") {
    auto result = akkado::compile(R"(
        const fn classify(x) -> match(x) { 0..10: 1, 10..100: 2, _: 3 }
        const c = classify(50)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 2.0f));
}

TEST_CASE("Const fn: match no arm matches returns 0", "[const]") {
    auto result = akkado::compile(R"(
        const fn lookup(x) -> match(x) { 1: 100, 2: 200 }
        const v = lookup(3)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 0.0f));
}

TEST_CASE("Const fn: match wildcard always matches", "[const]") {
    auto result = akkado::compile(R"(
        const fn always(x) -> match(x) { _: 42 }
        const v = always(99)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 42.0f));
}

// =============================================================================
// Group 5: Array Operations in Const
// =============================================================================

TEST_CASE("Const: array literal", "[const]") {
    auto result = akkado::compile("const freqs = [440, 880, 1320]");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 3);
    CHECK(has_const_approx(insts, 440.0f));
    CHECK(has_const_approx(insts, 880.0f));
    CHECK(has_const_approx(insts, 1320.0f));
}

TEST_CASE("Const: empty array", "[const]") {
    auto result = akkado::compile("const empty = []");
    REQUIRE(result.success);
}

TEST_CASE("Const: sum/len/mean of array", "[const]") {
    auto result = akkado::compile(R"(
        const a = [1, 2, 3, 4, 5]
        const s = sum(a)
        const l = len(a)
        const m = mean(a)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 15.0f));   // sum
    CHECK(has_const_approx(insts, 5.0f));    // len
    CHECK(has_const_approx(insts, 3.0f));    // mean
}

TEST_CASE("Const: map with closure", "[const]") {
    auto result = akkado::compile(R"(
        const fn scale_arr(xs, f) -> map(xs, (x) -> x * f)
        const v = scale_arr([1, 2, 3], 100)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 100.0f));
    CHECK(has_const_approx(insts, 200.0f));
    CHECK(has_const_approx(insts, 300.0f));
}

TEST_CASE("Const: map then sum (manual fold)", "[const]") {
    // fold has a builtin conflict (audio fold vs array fold), so test
    // the equivalent operation using map + sum
    auto result = akkado::compile(R"(
        const v = sum(map(range(1, 6), (x) -> x * x))
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // sum of squares 1..5 = 1+4+9+16+25 = 55
    CHECK(has_const_approx(insts, 55.0f));
}

TEST_CASE("Const: reversed range", "[const]") {
    auto result = akkado::compile("const r = range(5, 0)");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 5.0f));
    CHECK(has_const_approx(insts, 4.0f));
    CHECK(has_const_approx(insts, 3.0f));
    CHECK(has_const_approx(insts, 2.0f));
    CHECK(has_const_approx(insts, 1.0f));
}

TEST_CASE("Const: range with positive step", "[const]") {
    auto result = akkado::compile("const r = range(0, 10, 2)");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 0.0f));
    CHECK(has_const_approx(insts, 2.0f));
    CHECK(has_const_approx(insts, 4.0f));
    CHECK(has_const_approx(insts, 6.0f));
    CHECK(has_const_approx(insts, 8.0f));
}

// =============================================================================
// Group 6: Pitch Literals
// =============================================================================

TEST_CASE("Const: pitch literal in const", "[const]") {
    // Pitch literals in Akkado use quotes: 'C4'
    auto result = akkado::compile(R"(
        const freq = 'C4'
        sine(freq)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 261.626f, 0.1f));
}

TEST_CASE("Const fn: pitch literal as argument", "[const]") {
    auto result = akkado::compile(R"(
        const fn octave_up(f) -> f * 2
        const v = octave_up('A4')
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 880.0f, 0.1f));
}

// =============================================================================
// Group 7: Error Cases
// =============================================================================

TEST_CASE("Const: string literal rejected", "[const]") {
    auto result = akkado::compile(R"(const name = "hello")");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const: record literal rejected", "[const]") {
    auto result = akkado::compile("const r = {freq: 440}");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const: reassignment rejected", "[const]") {
    auto result = akkado::compile(R"(
        const x = 5
        const x = 10
    )");
    CHECK_FALSE(result.success);
    CHECK(has_error_code(result.diagnostics, "E150"));
}

TEST_CASE("Const fn: too few arguments", "[const]") {
    auto result = akkado::compile(R"(
        const fn f(a, b, c) -> a + b + c
        const v = f(1, 2)
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const: non-const variable in const expr", "[const]") {
    auto result = akkado::compile(R"(
        x = param("f", 440)
        const y = x + 1
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const fn: div by zero in const fn call", "[const]") {
    auto result = akkado::compile(R"(
        const fn inv(x) -> 1 / x
        const v = inv(0)
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("Const: mod not a builtin (fails E004)", "[const]") {
    // mod is in const_eval but NOT registered as a builtin function
    // This correctly fails at analysis time
    auto result = akkado::compile("const x = mod(5, 3)");
    CHECK_FALSE(result.success);
}

// =============================================================================
// Group 8: Integration with Runtime Code
// =============================================================================

TEST_CASE("Const: used as osc frequency", "[const]") {
    auto result = akkado::compile(R"(
        const f = 440
        sine(f) |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 440.0f));
}

TEST_CASE("Const: used in multiply", "[const]") {
    auto result = akkado::compile(R"(
        const gain = 0.5
        sine(440) |> % * gain |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 0.5f));
}

TEST_CASE("Const: array used as osc input", "[const]") {
    // Multi-voice arrays into a mono `out` need explicit summing — same rule
    // that `chord("Am") |> osc(...) |> out(...)` enforces in test_chord.cpp.
    // (Pre-Bug-A-fix this compiled because chord-expansion was silently
    // dropping voices 2..N; now that it correctly identifies all three voices,
    // the program needs a `sum()` step to be well-typed.)
    auto result = akkado::compile(R"(
        const freqs = [220, 440, 660]
        sine(freqs) |> sum(%) |> out(%, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 220.0f));
    CHECK(has_const_approx(insts, 440.0f));
    CHECK(has_const_approx(insts, 660.0f));
}

TEST_CASE("Const fn: with runtime arg falls back to inline", "[const]") {
    auto result = akkado::compile(R"(
        const fn dbl(x) -> x * 2
        f = param("f", 440)
        sine(dbl(f)) |> out(%, %)
    )");
    REQUIRE(result.success);
}

// =============================================================================
// Group 9: Edge Cases
// =============================================================================

TEST_CASE("Const: value is zero", "[const]") {
    auto result = akkado::compile(R"(
        const z = 0
        const zz = z * 100
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 0.0f));
}

TEST_CASE("Const: parenthesized expression", "[const]") {
    auto result = akkado::compile("const x = (2 + 3) * 4");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 20.0f));
}

TEST_CASE("Const: manual lerp expression", "[const]") {
    // lerp is not a registered builtin, so test the equivalent manually
    auto result = akkado::compile("const v = 0 + (100 - 0) * 0.25");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 25.0f));
}

TEST_CASE("Const: select as conditional (3-arg)", "[const]") {
    // select(cond, then, else) is the conditional builtin
    auto result = akkado::compile("const v = select(1, 42, 99)");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, 42.0f));
}

TEST_CASE("Const: negative literal", "[const]") {
    auto result = akkado::compile("const x = -42");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(has_const_approx(insts, -42.0f));
}

TEST_CASE("Const: unimplemented whitelisted fn fails", "[const][bug3]") {
    // reverse passes purity check but is NOT implemented in ConstEvaluator
    // This should fail at analysis (purity) or evaluation, not silently pass
    auto result = akkado::compile("const x = reverse(range(0, 5))");
    CHECK_FALSE(result.success);
}

// =============================================================================
// Const: Expression defaults in const fn calls
// =============================================================================

TEST_CASE("Const: expression default in const fn", "[const]") {
    SECTION("arithmetic expression default") {
        auto result = akkado::compile(R"(
            const fn f(x, cut = 440 * 2) -> x + cut
            const y = f(1)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // f(1) = 1 + 880 = 881
        CHECK(has_const_approx(insts, 881.0f));
    }

    SECTION("const fn call in default") {
        auto result = akkado::compile(R"(
            const fn double(x) -> x * 2
            const fn f(x, cut = double(440)) -> x + cut
            const y = f(1)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // f(1) = 1 + 880 = 881
        CHECK(has_const_approx(insts, 881.0f));
    }

    SECTION("expression default overridden by explicit arg") {
        auto result = akkado::compile(R"(
            const fn f(x, cut = 440 * 2) -> x + cut
            const y = f(1, 100)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // f(1, 100) = 1 + 100 = 101
        CHECK(has_const_approx(insts, 101.0f));
    }
}

// =============================================================================
// Const: Expression default interactions (F6 integration)
// =============================================================================

TEST_CASE("Const: expression default interactions", "[const]") {
    SECTION("nested const fn with expr default") {
        auto result = akkado::compile(R"(
            const fn base(oct = 4) -> 440 * pow(2, oct - 4)
            const fn freq(note = 0, oct = 4) -> base(oct) * pow(2, note / 12)
            const v = freq()
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // freq() -> base(4) * pow(2, 0/12) -> 440 * pow(2,0) * pow(2,0) -> 440
        CHECK(has_const_approx(insts, 440.0f));
    }

    SECTION("expr default with const variable") {
        auto result = akkado::compile(R"(
            const S = pow(2, 1.0 / 12)
            const fn up(f, steps = 1) -> f * pow(S, steps)
            const v = up(440)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // up(440) = 440 * pow(pow(2,1/12), 1) = 440 * 2^(1/12) ≈ 466.16
        CHECK(has_const_approx(insts, 466.16f, 0.1f));
    }

    SECTION("parenthesized expr default") {
        auto result = akkado::compile(R"(
            const fn f(x, factor = (1 + 2) * 3) -> x * factor
            const v = f(10)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // f(10) = 10 * 9 = 90
        CHECK(has_const_approx(insts, 90.0f));
    }

    SECTION("negation default in const fn") {
        auto result = akkado::compile(R"(
            const fn offset(x, y = -1) -> x + y
            const v = offset(10)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // offset(10) = 10 + (-1) = 9
        CHECK(has_const_approx(insts, 9.0f));
    }

    SECTION("math builtin in default") {
        auto result = akkado::compile(R"(
            const fn f(x, cut = 440 * pow(2, 3)) -> x + cut
            const v = f(0)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // f(0) = 0 + 440*8 = 3520
        CHECK(has_const_approx(insts, 3520.0f));
    }

    SECTION("default overridden in nested call") {
        auto result = akkado::compile(R"(
            const fn scale(x, f = 2 * 3) -> x * f
            const fn apply(x) -> scale(x, 10)
            const v = apply(5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // apply(5) -> scale(5, 10) -> 5 * 10 = 50
        CHECK(has_const_approx(insts, 50.0f));
    }

    SECTION("multiple defaults partial override") {
        auto result = akkado::compile(R"(
            const fn f(x, a = 1 + 1, b = 2 * 3) -> x + a + b
            const v = f(100, 5)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // f(100, 5) = 100 + 5 + 6 = 111
        CHECK(has_const_approx(insts, 111.0f));
    }

    SECTION("default with select/conditional") {
        auto result = akkado::compile(R"(
            const fn f(x, mode = select(1 > 0, 2, 3)) -> x * mode
            const v = f(10)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // select(true, 2, 3) -> 2; f(10) = 10 * 2 = 20
        CHECK(has_const_approx(insts, 20.0f));
    }
}
