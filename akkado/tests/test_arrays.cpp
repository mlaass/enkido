#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <set>
#include <vector>

using Catch::Approx;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// =============================================================================
// Test helpers (mirrors test_codegen.cpp patterns)
// =============================================================================

static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

static std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    if (count > 0) {
        std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    }
    return instructions;
}

static std::size_t count_instructions(const std::vector<cedar::Instruction>& insts,
                                       cedar::Opcode op) {
    std::size_t count = 0;
    for (const auto& inst : insts) {
        if (inst.opcode == op) ++count;
    }
    return count;
}

// Collect all PUSH_CONST values in instruction order
static std::vector<float> collect_consts(const std::vector<cedar::Instruction>& insts) {
    std::vector<float> values;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::PUSH_CONST) {
            values.push_back(decode_const_float(inst));
        }
    }
    return values;
}

static akkado::CompileResult must_compile(const std::string& src) {
    auto result = akkado::compile(src);
    if (!result.success) {
        for (const auto& d : result.diagnostics) {
            UNSCOPED_INFO("Diagnostic: " << d.code << ": " << d.message);
        }
    }
    REQUIRE(result.success);
    return result;
}

// =============================================================================
// Array literals
// =============================================================================

TEST_CASE("Array literals: basic forms compile", "[arrays][literals]") {
    SECTION("empty array") {
        auto result = must_compile("[]");
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(insts[0].opcode == cedar::Opcode::PUSH_CONST);
        CHECK(decode_const_float(insts[0]) == 0.0f);
    }

    SECTION("single number") {
        auto result = must_compile("[42]");
        auto insts = get_instructions(result);
        REQUIRE(insts.size() == 1);
        CHECK(decode_const_float(insts[0]) == 42.0f);
    }

    SECTION("multiple numbers") {
        auto result = must_compile("[1, 2, 3, 4]");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() == 4);
        CHECK(values[0] == 1.0f);
        CHECK(values[1] == 2.0f);
        CHECK(values[2] == 3.0f);
        CHECK(values[3] == 4.0f);
    }

    SECTION("mixed types") {
        // Numbers, strings, and bools coexist as literal entries.
        auto result = akkado::compile("[1, \"hello\", true]");
        REQUIRE(result.success);
    }

    SECTION("nested arrays") {
        auto result = akkado::compile("[[1, 2], [3, 4]]");
        REQUIRE(result.success);
    }

    SECTION("trailing comma is rejected") {
        auto result = akkado::compile("[1, 2, 3,]");
        CHECK_FALSE(result.success);
    }

    SECTION("expressions inside literals") {
        auto result = must_compile("[1 + 2, 3 * 4]");
        // 1+2 = 3 (compile-time folded), 3*4 = 12 (compile-time folded)
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 2);
    }
}

// =============================================================================
// Indexing
// =============================================================================

TEST_CASE("Indexing: compile-time index", "[arrays][indexing]") {
    SECTION("first element") {
        auto result = must_compile("[10, 20, 30][0]");
        auto values = collect_consts(get_instructions(result));
        // The index resolves at compile time — element 10 is referenced.
        CHECK(std::find(values.begin(), values.end(), 10.0f) != values.end());
    }

    SECTION("middle element") {
        auto result = must_compile("[10, 20, 30][1]");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 20.0f) != values.end());
    }

    SECTION("last element") {
        auto result = must_compile("[10, 20, 30][2]");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 30.0f) != values.end());
    }
}

TEST_CASE("Indexing: negative index wraps modulo length", "[arrays][indexing]") {
    SECTION("-1 returns last element") {
        auto result = must_compile("[10, 20, 30][-1]");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 30.0f) != values.end());
    }

    SECTION("-2 returns middle element") {
        auto result = must_compile("[10, 20, 30][-2]");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 20.0f) != values.end());
    }

    SECTION("large negative wraps") {
        // -7 mod 3 = 2 (since codegen does ((idx%len)+len)%len)
        // -7 % 3 = -1 (C++), then (-1 + 3) % 3 = 2 → element 30
        auto result = must_compile("[10, 20, 30][-7]");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 30.0f) != values.end());
    }
}

TEST_CASE("Indexing: out-of-bounds wraps modulo length", "[arrays][indexing]") {
    SECTION("index == length wraps to 0") {
        auto result = must_compile("[10, 20, 30][3]");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 10.0f) != values.end());
    }

    SECTION("large index wraps") {
        // 7 % 3 = 1 → element 20
        auto result = must_compile("[10, 20, 30][7]");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 20.0f) != values.end());
    }
}

TEST_CASE("Indexing: runtime index emits ARRAY_INDEX", "[arrays][indexing]") {
    // A non-literal index forces runtime indexing via ARRAY_PACK + ARRAY_INDEX.
    auto result = must_compile(
        "xs = [220, 440, 880]\n"
        "idx = lfo(0.5)\n"
        "xs[idx] |> out(%, %)");

    auto insts = get_instructions(result);
    bool has_pack = count_instructions(insts, cedar::Opcode::ARRAY_PACK) > 0;
    bool has_index = count_instructions(insts, cedar::Opcode::ARRAY_INDEX) > 0;
    CHECK(has_pack);
    CHECK(has_index);
}

// =============================================================================
// len
// =============================================================================

TEST_CASE("len: returns compile-time array length", "[arrays][len]") {
    SECTION("simple length") {
        auto result = must_compile("len([1, 2, 3, 4])");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 4.0f) != values.end());
    }

    SECTION("empty array") {
        auto result = must_compile("len([])");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 0.0f) != values.end());
    }

    SECTION("non-array argument is an error") {
        auto result = akkado::compile("len(42)");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// map
// =============================================================================

TEST_CASE("map: applies function to each element", "[arrays][map]") {
    SECTION("identity closure routes input through unchanged") {
        // Regression for the bare-identifier closure body bug: the broken
        // walker mis-classified `h` as a phantom param, leaving body =
        // NULL_NODE and apply_function_ref returning BUFFER_UNUSED. PUSH_CONST
        // count alone (3) cannot catch the silent failure — pipe the result
        // through reduce() so the sum's MUL/ADD chain materializes only when
        // the closure body actually returns its argument.
        auto result = must_compile(
            "map([1.5, 2.5, 3.5], (h) -> h) |> reduce(@, (a, b) -> a + b, 0)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::PUSH_CONST) >= 3);
        // 3 elements -> 3 ADDs through reduce (init+xs[0], result+xs[1], …).
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("multiplication broadcasts to each element") {
        auto result = must_compile("map([1, 2, 3], (x) -> x * 2)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
    }

    SECTION("user fn with bare-identifier body round-trips") {
        // `fn id(x) -> x` is the user-fn variant of the identity closure;
        // its body is a bare Identifier. Exercises handle_closure /
        // user-fn-inline-expansion walkers (codegen_functions.cpp:~1555,
        // ~1620), distinct from apply_function_ref in codegen_arrays.cpp.
        // Call-form, not pipe-form: a bare `[…]` on a new line after `id`
        // would parse as `id[…]` index-into-fn (P001).
        auto result = must_compile(
            "fn id(x) -> x\n"
            "map([1.5, 2.5, 3.5], id) |> reduce(@, (a, b) -> a + b, 0)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("each voice gets a unique state_id when stateful") {
        auto result = must_compile(
            "[440, 550, 660] |> map(%, (f) -> osc(\"sin\", f)) |> sum(%) |> out(%, %)");
        auto insts = get_instructions(result);

        std::set<std::uint32_t> state_ids;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::OSC_SIN) {
                state_ids.insert(inst.state_id);
            }
        }
        CHECK(state_ids.size() == 3);
    }
}

// =============================================================================
// reduce
// =============================================================================
// The higher-order reducer is named 'reduce' (not 'fold') because 'fold' is
// already taken by the wavefolding distortion builtin.

TEST_CASE("reduce: reduces array with binary function", "[arrays][reduce]") {
    SECTION("sum via reduce") {
        auto result = must_compile("reduce([1, 2, 3, 4], (acc, x) -> acc + x, 0)");
        auto insts = get_instructions(result);
        // 4 elements → 4 ADDs (init+xs[0], result+xs[1], ...)
        CHECK(count_instructions(insts, cedar::Opcode::ADD) >= 4);
    }

    SECTION("empty array returns init unchanged") {
        auto result = must_compile("reduce([], (acc, x) -> acc + x, 7)");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 7.0f) != values.end());
    }

    SECTION("single-element array applies function once") {
        auto result = must_compile("reduce([5], (acc, x) -> acc * x, 2)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 1);
    }
}

// =============================================================================
// zipWith
// =============================================================================

TEST_CASE("zipWith: combines two arrays element-wise", "[arrays][zipWith]") {
    SECTION("equal-length addition") {
        auto result = must_compile("zipWith([1, 2, 3], [10, 20, 30], (a, b) -> a + b)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("mismatched lengths truncate to shorter") {
        auto result = must_compile("zipWith([1, 2, 3, 4, 5], [10, 20], (a, b) -> a + b)");
        auto insts = get_instructions(result);
        // Only 2 ADDs because input b is the shorter array (length 2)
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("first array shorter") {
        auto result = must_compile("zipWith([1, 2], [10, 20, 30, 40], (a, b) -> a + b)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("empty inputs produce zero result") {
        auto result = must_compile("zipWith([], [], (a, b) -> a + b)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }
}

// =============================================================================
// zip
// =============================================================================

TEST_CASE("zip: interleaves two arrays", "[arrays][zip]") {
    SECTION("equal length produces 2N elements") {
        auto result = must_compile("zip([1, 2, 3], [10, 20, 30])");
        auto values = collect_consts(get_instructions(result));
        // Original 6 PUSH_CONSTs (3 + 3), interleaved access pattern
        CHECK(values.size() == 6);
    }

    SECTION("mismatched lengths truncate to shorter") {
        auto result = akkado::compile(
            "zip([1, 2, 3, 4, 5], [10, 20]) |> sum(%)");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // Result array has 4 elements (2 pairs interleaved); sum chains 3 ADDs
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }
}

// =============================================================================
// take / drop
// =============================================================================

TEST_CASE("take: keeps first n elements", "[arrays][take]") {
    SECTION("normal take") {
        auto result = must_compile("sum(take(2, [10, 20, 30, 40]))");
        auto insts = get_instructions(result);
        // Sum of two elements = 1 ADD
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 1);
    }

    SECTION("take more than length returns entire array") {
        auto result = must_compile("sum(take(99, [1, 2, 3]))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);  // 3 elems → 2 ADDs
    }

    SECTION("take 0 returns empty") {
        auto result = must_compile("sum(take(0, [1, 2, 3]))");
        auto insts = get_instructions(result);
        // Empty sum returns 0; no ADDs
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }
}

TEST_CASE("drop: skips first n elements", "[arrays][drop]") {
    SECTION("normal drop") {
        auto result = must_compile("sum(drop(2, [10, 20, 30, 40]))");
        auto insts = get_instructions(result);
        // Two elements remain → 1 ADD
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 1);
    }

    SECTION("drop everything returns empty") {
        auto result = must_compile("sum(drop(5, [1, 2, 3]))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("drop 0 returns full array") {
        auto result = must_compile("sum(drop(0, [1, 2, 3]))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }
}

// =============================================================================
// reverse
// =============================================================================

TEST_CASE("reverse: reverses element order", "[arrays][reverse]") {
    SECTION("multiple elements") {
        // After reverse, sum is identical, but the order matters when we map.
        auto result = must_compile("sum(reverse([1, 2, 3]))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("single element is a no-op") {
        auto result = must_compile("reverse([42])");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 42.0f) != values.end());
    }
}

// =============================================================================
// sum
// =============================================================================

TEST_CASE("sum: reduces array with addition", "[arrays][sum]") {
    SECTION("multi-element") {
        auto result = must_compile("sum([1, 2, 3])");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("single element is itself") {
        auto result = must_compile("sum([42])");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("empty array is zero") {
        auto result = must_compile("sum([])");
        auto insts = get_instructions(result);
        // No ADDs because there are no elements to sum.
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
        // A zero constant is emitted somewhere in the program.
        auto values = collect_consts(insts);
        CHECK(std::find(values.begin(), values.end(), 0.0f) != values.end());
    }

    SECTION("8-element sum reuses one accumulator buffer") {
        // The in-place accumulator emits N-1 ADDs but writes them all
        // into the same output buffer, instead of allocating a fresh
        // temp per ADD. A linear chain would burn 7 distinct buffers
        // for 8 elements; we expect exactly 1.
        auto result = must_compile("sum([1, 2, 3, 4, 5, 6, 7, 8])");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 7);
        std::set<std::uint16_t> add_outs;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::ADD) {
                add_outs.insert(inst.out_buffer);
            }
        }
        CHECK(add_outs.size() == 1);
    }
}

// =============================================================================
// product via reduce(*, 1)
// =============================================================================
// There is no dedicated 'product' builtin — express it as
//   reduce(xs, (a, b) -> a * b, 1)
// which is the canonical multiplicative-fold form.

TEST_CASE("reduce as product: multiply via fold", "[arrays][reduce]") {
    SECTION("multi-element product") {
        auto result = must_compile("reduce([2, 3, 4], (a, b) -> a * b, 1)");
        auto insts = get_instructions(result);
        // 1 * 2 * 3 * 4 — three MULs (one per element)
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 3);
    }

    SECTION("empty array returns identity (1)") {
        auto result = must_compile("reduce([], (a, b) -> a * b, 1)");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 1.0f) != values.end());
    }
}

// =============================================================================
// mean
// =============================================================================

TEST_CASE("mean: arithmetic average", "[arrays][mean]") {
    SECTION("multi-element") {
        auto result = must_compile("mean([2, 4, 6])");
        auto insts = get_instructions(result);
        // Sum (2 ADDs) + divide-by-length (1 DIV)
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
        CHECK(count_instructions(insts, cedar::Opcode::DIV) == 1);
        // Both ADDs share the same accumulator output buffer.
        std::set<std::uint16_t> add_outs;
        for (const auto& inst : insts) {
            if (inst.opcode == cedar::Opcode::ADD) {
                add_outs.insert(inst.out_buffer);
            }
        }
        CHECK(add_outs.size() == 1);
    }

    SECTION("single element returns element") {
        auto result = must_compile("mean([5])");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
        CHECK(count_instructions(insts, cedar::Opcode::DIV) == 0);
    }

    SECTION("empty array returns 0") {
        auto result = must_compile("mean([])");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 0.0f) != values.end());
    }
}

// =============================================================================
// rotate
// =============================================================================

TEST_CASE("rotate: shifts elements by n positions", "[arrays][rotate]") {
    SECTION("rotate right by 1") {
        // [1,2,3,4] rotated right by 1 = [4,1,2,3]; sum unchanged
        auto result = must_compile("sum(rotate([1, 2, 3, 4], 1))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("rotate by 0 is no-op") {
        auto result = must_compile("rotate([1, 2, 3], 0)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 3);
    }

    SECTION("negative rotation rotates left") {
        auto result = must_compile("sum(rotate([1, 2, 3, 4], -1))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("rotation greater than length wraps") {
        auto result = must_compile("sum(rotate([1, 2, 3], 7))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 2);
    }

    SECTION("non-literal n is an error") {
        auto result = akkado::compile("n = 1\nrotate([1, 2, 3], n)");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// shuffle
// =============================================================================

TEST_CASE("shuffle: deterministic permutation", "[arrays][shuffle]") {
    SECTION("preserves element count") {
        auto result = must_compile("sum(shuffle([1, 2, 3, 4, 5]))");
        auto insts = get_instructions(result);
        // 5 elements → 4 ADDs regardless of order
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 4);
    }

    SECTION("two compiles in same code position produce identical output") {
        auto a = must_compile("shuffle([10, 20, 30, 40, 50])");
        auto b = must_compile("shuffle([10, 20, 30, 40, 50])");

        auto vals_a = collect_consts(get_instructions(a));
        auto vals_b = collect_consts(get_instructions(b));
        REQUIRE(vals_a.size() == vals_b.size());
        for (std::size_t i = 0; i < vals_a.size(); ++i) {
            CHECK(vals_a[i] == vals_b[i]);
        }
    }

    SECTION("single element passes through") {
        auto result = must_compile("shuffle([42])");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 42.0f) != values.end());
    }

    SECTION("seeded shuffle compiles and preserves element count") {
        auto result = must_compile("sum(shuffle([1, 2, 3, 4, 5], 7))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 4);
    }

    SECTION("non-constant seed is rejected") {
        auto result = akkado::compile("shuffle([1, 2, 3], freq)");
        REQUIRE_FALSE(result.success);
    }
}

// =============================================================================
// sort
// =============================================================================

TEST_CASE("sort: ascending order at compile time", "[arrays][sort]") {
    SECTION("already sorted") {
        auto result = must_compile("sort([1, 2, 3])");
        auto values = collect_consts(get_instructions(result));
        // The sorted result is emitted as new PUSH_CONSTs in ascending order.
        // Find the sorted values in instruction order.
        REQUIRE(values.size() >= 3);
    }

    SECTION("reverse-sorted input") {
        auto result = must_compile("sort([5, 3, 1, 4, 2])");
        auto values = collect_consts(get_instructions(result));
        // Last 5 PUSH_CONSTs should be the sorted output: 1, 2, 3, 4, 5
        REQUIRE(values.size() >= 5);
        // Tail should be ascending
        auto tail = std::vector<float>(values.end() - 5, values.end());
        for (std::size_t i = 1; i < tail.size(); ++i) {
            CHECK(tail[i] >= tail[i - 1]);
        }
    }

    SECTION("ties are preserved (stable result)") {
        auto result = must_compile("sort([3, 1, 3, 1, 2])");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 5);
        auto tail = std::vector<float>(values.end() - 5, values.end());
        CHECK(tail[0] == 1.0f);
        CHECK(tail[1] == 1.0f);
        CHECK(tail[2] == 2.0f);
        CHECK(tail[3] == 3.0f);
        CHECK(tail[4] == 3.0f);
    }

    SECTION("negative values sort correctly") {
        auto result = must_compile("sort([3, -1, 0, -5, 2])");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 5, values.end());
        CHECK(tail[0] == -5.0f);
        CHECK(tail[1] == -1.0f);
        CHECK(tail[2] == 0.0f);
        CHECK(tail[3] == 2.0f);
        CHECK(tail[4] == 3.0f);
    }

    SECTION("non-literal element passes through unchanged") {
        // Cannot sort at compile time when elements are runtime expressions.
        auto result = akkado::compile("sort([osc(\"sin\", 220), osc(\"sin\", 440)])");
        REQUIRE(result.success);
    }

    SECTION("reverse=true sorts descending") {
        auto result = must_compile("sort([3, 1, 4, 1, 5, 9, 2, 6], true)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 8);
        auto tail = std::vector<float>(values.end() - 8, values.end());
        CHECK(tail[0] == 9.0f);
        CHECK(tail[1] == 6.0f);
        CHECK(tail[2] == 5.0f);
        CHECK(tail[3] == 4.0f);
        CHECK(tail[4] == 3.0f);
        CHECK(tail[5] == 2.0f);
        CHECK(tail[6] == 1.0f);
        CHECK(tail[7] == 1.0f);
    }

    SECTION("reverse=false matches default ascending") {
        auto result = must_compile("sort([3, 1, 4, 1, 5], false)");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 5, values.end());
        CHECK(tail[0] == 1.0f);
        CHECK(tail[1] == 1.0f);
        CHECK(tail[2] == 3.0f);
        CHECK(tail[3] == 4.0f);
        CHECK(tail[4] == 5.0f);
    }

    SECTION("non-constant reverse argument is rejected") {
        auto result = akkado::compile("sort([3, 1, 2], freq)");
        REQUIRE_FALSE(result.success);
    }
}

// =============================================================================
// normalize
// =============================================================================

TEST_CASE("normalize: scales array to [0, 1]", "[arrays][normalize]") {
    SECTION("typical case emits min/max + per-element transform") {
        auto result = must_compile("sum(normalize([10, 20, 30]))");
        auto insts = get_instructions(result);
        // Each element: SUB (subtract min), DIV (divide by range)
        // Plus min/max search uses MIN/MAX/SUB.
        CHECK(count_instructions(insts, cedar::Opcode::DIV) >= 3);
        CHECK(count_instructions(insts, cedar::Opcode::SUB) >= 3);
    }

    SECTION("single element returns 0") {
        auto result = must_compile("normalize([42])");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 0.0f) != values.end());
    }

    SECTION("empty array returns 0") {
        auto result = must_compile("normalize([])");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 0.0f) != values.end());
    }

    SECTION("custom target range emits MUL+ADD per element") {
        auto result = must_compile("sum(normalize([10, 20, 30], -1, 1))");
        auto insts = get_instructions(result);
        // 3 elements: SUB (subtract min) + DIV + MUL (span) + ADD (lo) per element
        CHECK(count_instructions(insts, cedar::Opcode::DIV) >= 3);
        CHECK(count_instructions(insts, cedar::Opcode::MUL) >= 3);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) >= 3);
    }

    SECTION("default [0,1] target emits no extra MUL/ADD beyond min/max search") {
        auto result = must_compile("sum(normalize([10, 20, 30]))");
        auto insts = get_instructions(result);
        // Default path: only SUB/DIV per element (no MUL); the trailing
        // ADDs come from sum() (n-1 = 2), nothing from normalize.
        CHECK(count_instructions(insts, cedar::Opcode::MUL) == 0);
    }

    SECTION("hi <= lo is rejected") {
        auto bad = akkado::compile("normalize([1, 2, 3], 5, 1)");
        CHECK_FALSE(bad.success);
        auto eq = akkado::compile("normalize([1, 2, 3], 5, 5)");
        CHECK_FALSE(eq.success);
    }

    SECTION("non-constant lo/hi is rejected") {
        auto result = akkado::compile("normalize([1, 2, 3], 0, freq)");
        CHECK_FALSE(result.success);
    }

    // The old `scale(array, lo, hi)` builtin was removed in favor of
    // `normalize` (identical behavior). The `scale` name is now free for the
    // future scale-quantize transform — guard that it no longer resolves as
    // an array builtin.
    SECTION("removed scale() builtin does not compile") {
        auto result = akkado::compile("scale([1, 2, 3], 0, 1)");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// range
// =============================================================================

TEST_CASE("range: integer sequence", "[arrays][range]") {
    SECTION("ascending range") {
        auto result = must_compile("range(0, 4)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 4);
        // Last 4 PUSH_CONSTs are 0, 1, 2, 3
        auto tail = std::vector<float>(values.end() - 4, values.end());
        CHECK(tail[0] == 0.0f);
        CHECK(tail[1] == 1.0f);
        CHECK(tail[2] == 2.0f);
        CHECK(tail[3] == 3.0f);
    }

    SECTION("descending range when start > end") {
        auto result = must_compile("range(4, 0)");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 4, values.end());
        CHECK(tail[0] == 4.0f);
        CHECK(tail[1] == 3.0f);
        CHECK(tail[2] == 2.0f);
        CHECK(tail[3] == 1.0f);
    }

    SECTION("equal start and end produces empty array") {
        auto result = must_compile("sum(range(2, 2))");
        auto insts = get_instructions(result);
        // Empty sum → no ADDs
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("ascending range with step > 1") {
        auto result = must_compile("range(0, 10, 2)");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 5, values.end());
        CHECK(tail[0] == 0.0f);
        CHECK(tail[1] == 2.0f);
        CHECK(tail[2] == 4.0f);
        CHECK(tail[3] == 6.0f);
        CHECK(tail[4] == 8.0f);
    }

    SECTION("descending range with step > 1 (start > end auto-reverses)") {
        auto result = must_compile("range(10, 0, 2)");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 5, values.end());
        CHECK(tail[0] == 10.0f);
        CHECK(tail[1] == 8.0f);
        CHECK(tail[2] == 6.0f);
        CHECK(tail[3] == 4.0f);
        CHECK(tail[4] == 2.0f);
    }

    SECTION("negative step value uses magnitude (direction from start/end)") {
        auto result = must_compile("range(0, 10, -2)");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 5, values.end());
        CHECK(tail[0] == 0.0f);
        CHECK(tail[1] == 2.0f);
        CHECK(tail[2] == 4.0f);
        CHECK(tail[3] == 6.0f);
        CHECK(tail[4] == 8.0f);
    }

    SECTION("step=0 is rejected") {
        auto result = akkado::compile("range(0, 4, 0)");
        REQUIRE_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E153") found = true;
        }
        CHECK(found);
    }
}

// =============================================================================
// repeat
// =============================================================================

TEST_CASE("repeat: replicates a value n times", "[arrays][repeat]") {
    SECTION("normal repeat") {
        auto result = must_compile("sum(repeat(5, 4))");
        auto insts = get_instructions(result);
        // 4 references to one value, 3 ADDs
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("repeat 1 returns single value") {
        auto result = must_compile("repeat(7, 1)");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 7.0f) != values.end());
    }

    SECTION("repeat 0 returns empty") {
        auto result = must_compile("sum(repeat(5, 0))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("non-literal n is an error") {
        auto result = akkado::compile("n = 4\nrepeat(5, n)");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// linspace
// =============================================================================

TEST_CASE("linspace: evenly spaced values, inclusive endpoints", "[arrays][linspace]") {
    SECTION("inclusive on both ends") {
        auto result = must_compile("linspace(0, 1, 5)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 5);
        auto tail = std::vector<float>(values.end() - 5, values.end());
        CHECK_THAT(tail[0], WithinAbs(0.0f, 1e-5));
        CHECK_THAT(tail[1], WithinAbs(0.25f, 1e-5));
        CHECK_THAT(tail[2], WithinAbs(0.5f, 1e-5));
        CHECK_THAT(tail[3], WithinAbs(0.75f, 1e-5));
        CHECK_THAT(tail[4], WithinAbs(1.0f, 1e-5));
    }

    SECTION("n=1 returns [start]") {
        auto result = must_compile("linspace(7, 100, 1)");
        auto values = collect_consts(get_instructions(result));
        CHECK(std::find(values.begin(), values.end(), 7.0f) != values.end());
    }

    SECTION("n=2 returns [start, end]") {
        auto result = must_compile("linspace(10, 20, 2)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 2);
        auto tail = std::vector<float>(values.end() - 2, values.end());
        CHECK(tail[0] == 10.0f);
        CHECK(tail[1] == 20.0f);
    }

    SECTION("n=0 returns empty") {
        auto result = must_compile("sum(linspace(0, 1, 0))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("non-literal n is an error") {
        auto result = akkado::compile("n = 5\nlinspace(0, 1, n)");
        CHECK_FALSE(result.success);
    }

    SECTION("explicit \"linear\" mode matches default") {
        auto a = must_compile("linspace(0, 1, 5)");
        auto b = must_compile("linspace(0, 1, 5, \"linear\")");
        auto va = collect_consts(get_instructions(a));
        auto vb = collect_consts(get_instructions(b));
        REQUIRE(va.size() == vb.size());
        for (std::size_t i = 0; i < va.size(); ++i) CHECK(va[i] == vb[i]);
    }

    SECTION("log mode produces a geometric frequency sweep") {
        auto result = must_compile("linspace(20, 20000, 4, \"log\")");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 4);
        auto tail = std::vector<float>(values.end() - 4, values.end());
        // Endpoints exact, middle values are 20*1000^(1/3) and 20*1000^(2/3)
        // = ~200 and ~2000.
        CHECK_THAT(tail[0], WithinAbs(20.0f, 1e-2f));
        CHECK_THAT(tail[1], WithinAbs(200.0f, 1.0f));
        CHECK_THAT(tail[2], WithinAbs(2000.0f, 10.0f));
        CHECK_THAT(tail[3], WithinAbs(20000.0f, 1.0f));
    }

    SECTION("geom mode is an alias for log mode") {
        auto a = must_compile("linspace(1, 64, 7, \"log\")");
        auto b = must_compile("linspace(1, 64, 7, \"geom\")");
        auto va = collect_consts(get_instructions(a));
        auto vb = collect_consts(get_instructions(b));
        REQUIRE(va.size() == vb.size());
        for (std::size_t i = 0; i < va.size(); ++i) {
            CHECK_THAT(va[i], WithinAbs(vb[i], 1e-3f));
        }
    }

    SECTION("unknown mode is rejected") {
        auto result = akkado::compile("linspace(0, 1, 5, \"cheesy\")");
        CHECK_FALSE(result.success);
    }

    SECTION("log mode with start <= 0 is rejected") {
        auto bad = akkado::compile("linspace(0, 100, 5, \"log\")");
        CHECK_FALSE(bad.success);
        auto neg = akkado::compile("linspace(-1, 100, 5, \"log\")");
        CHECK_FALSE(neg.success);
    }

    SECTION("non-string mode is rejected") {
        auto result = akkado::compile("linspace(0, 1, 5, 0)");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// random
// =============================================================================

TEST_CASE("random: deterministic random values", "[arrays][random]") {
    SECTION("produces requested count") {
        auto result = must_compile("sum(random(4))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 3);
    }

    SECTION("values lie in [0, 1)") {
        auto result = must_compile("random(8)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 8);
        auto tail = std::vector<float>(values.end() - 8, values.end());
        for (float v : tail) {
            CHECK(v >= 0.0f);
            CHECK(v < 1.0f);
        }
    }

    SECTION("same code position yields identical values") {
        auto a = must_compile("random(6)");
        auto b = must_compile("random(6)");
        auto vals_a = collect_consts(get_instructions(a));
        auto vals_b = collect_consts(get_instructions(b));
        REQUIRE(vals_a.size() == vals_b.size());
        for (std::size_t i = 0; i < vals_a.size(); ++i) {
            CHECK(vals_a[i] == vals_b[i]);
        }
    }

    SECTION("n=0 returns empty array") {
        auto result = must_compile("sum(random(0))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("values lie in [min, max) when range is given") {
        auto result = must_compile("random(8, 60, 72)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 8);
        auto tail = std::vector<float>(values.end() - 8, values.end());
        for (float v : tail) {
            CHECK(v >= 60.0f);
            CHECK(v < 72.0f);
        }
    }

    SECTION("negative range is supported (min < 0)") {
        auto result = must_compile("random(8, -1, 1)");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 8, values.end());
        for (float v : tail) {
            CHECK(v >= -1.0f);
            CHECK(v < 1.0f);
        }
    }

    SECTION("max <= min is rejected") {
        auto bad = akkado::compile("random(4, 10, 5)");
        CHECK_FALSE(bad.success);
        auto equal = akkado::compile("random(4, 5, 5)");
        CHECK_FALSE(equal.success);
    }

    SECTION("non-constant min/max is rejected") {
        auto result = akkado::compile("random(4, 0, freq)");
        CHECK_FALSE(result.success);
    }
}

// =============================================================================
// harmonics
// =============================================================================

TEST_CASE("harmonics: harmonic series including fundamental", "[arrays][harmonics]") {
    SECTION("first element is the fundamental") {
        auto result = must_compile("harmonics(110, 4)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 4);
        auto tail = std::vector<float>(values.end() - 4, values.end());
        CHECK(tail[0] == 110.0f);   // fundamental × 1
        CHECK(tail[1] == 220.0f);   // × 2
        CHECK(tail[2] == 330.0f);   // × 3
        CHECK(tail[3] == 440.0f);   // × 4
    }

    SECTION("n=0 returns empty") {
        auto result = must_compile("sum(harmonics(110, 0))");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::ADD) == 0);
    }

    SECTION("non-literal arguments are an error") {
        auto result = akkado::compile("f = 110\nharmonics(f, 4)");
        CHECK_FALSE(result.success);
    }

    SECTION("ratio=1.0 matches the default integer-multiplier path") {
        auto a = must_compile("harmonics(110, 4)");
        auto b = must_compile("harmonics(110, 4, 1.0)");
        auto vals_a = collect_consts(get_instructions(a));
        auto vals_b = collect_consts(get_instructions(b));
        REQUIRE(vals_a.size() >= 4);
        REQUIRE(vals_b.size() >= 4);
        auto tail_a = std::vector<float>(vals_a.end() - 4, vals_a.end());
        auto tail_b = std::vector<float>(vals_b.end() - 4, vals_b.end());
        CHECK(tail_a == tail_b);
    }

    SECTION("ratio>1 stretches partials sharper than integer multiples") {
        auto result = must_compile("harmonics(110, 4, 1.5)");
        auto values = collect_consts(get_instructions(result));
        REQUIRE(values.size() >= 4);
        auto tail = std::vector<float>(values.end() - 4, values.end());
        // Fundamental unchanged
        CHECK_THAT(tail[0], WithinAbs(110.0f, 1e-3f));
        // Higher partials should be sharper than n*f0
        CHECK(tail[1] > 220.0f);
        CHECK(tail[2] > 330.0f);
        CHECK(tail[3] > 440.0f);
    }

    SECTION("ratio<1 compresses partials below integer multiples") {
        auto result = must_compile("harmonics(110, 4, 0.5)");
        auto values = collect_consts(get_instructions(result));
        auto tail = std::vector<float>(values.end() - 4, values.end());
        CHECK_THAT(tail[0], WithinAbs(110.0f, 1e-3f));
        // Higher partials should be flatter than n*f0
        CHECK(tail[1] < 220.0f);
        CHECK(tail[2] < 330.0f);
        CHECK(tail[3] < 440.0f);
    }

    SECTION("ratio <= -1 is rejected") {
        auto bad = akkado::compile("harmonics(110, 4, -1)");
        CHECK_FALSE(bad.success);
        auto worse = akkado::compile("harmonics(110, 4, -2)");
        CHECK_FALSE(worse.success);
    }
}

// =============================================================================
// Integration: arrays in audio pipelines
// =============================================================================

TEST_CASE("Arrays compose into multi-voice audio graphs", "[arrays][integration]") {
    SECTION("map + sum produces N oscillators") {
        auto result = must_compile(
            "[220, 330, 440, 550] |> map(%, (f) -> osc(\"saw\", f)) |> sum(%) |> out(%, %)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SAW) == 4);
    }

    SECTION("zipWith + sum applies per-voice gain") {
        auto result = must_compile(
            "freqs = [220, 330, 440]\n"
            "gains = [1.0, 0.7, 0.5]\n"
            "zipWith(freqs, gains, (f, g) -> osc(\"sin\", f) * g) |> sum(%) |> out(%, %)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 3);
    }

    SECTION("harmonics + map drives additive synthesis") {
        auto result = must_compile(
            "harmonics(110, 8) |> map(%, (f) -> osc(\"sin\", f)) |> sum(%) * 0.1 |> out(%, %)");
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::OSC_SIN) == 8);
    }
}
