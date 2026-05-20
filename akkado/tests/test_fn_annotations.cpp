// Tests for fn annotations and recursion rejection — PRD
// prd-runtime-functions-control-flow L2.
//
// L2 in this build covers the surface-language half of "callable blocks":
//   - the `#inline` annotation (lexer `#` token + statement-position parsing),
//   - rejection of recursive `fn` definitions (E240 / E244).
// The subprogram-table / BLOCK_CALL machinery is deferred to L3, where
// FOREACH_EVENT dispatch gives it a concrete consumer. Under the chosen
// compile-time-expansion model, `#inline fn` and a default `fn` emit
// byte-identical bytecode — that invariant is asserted directly below.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <vector>

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts;
    insts.resize(r.bytecode.size() / sizeof(cedar::Instruction));
    std::memcpy(insts.data(), r.bytecode.data(), r.bytecode.size());
    return insts;
}

size_t count_op(const std::vector<cedar::Instruction>& insts, cedar::Opcode op) {
    size_t n = 0;
    for (const auto& i : insts) if (i.opcode == op) ++n;
    return n;
}

bool has_diag(const akkado::CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) if (d.code == code) return true;
    return false;
}

}  // namespace

// =============================================================================
// #inline annotation — parsing
// =============================================================================

TEST_CASE("#inline fn compiles and works", "[fn_annotations][inline]") {
    auto r = akkado::compile("#inline fn dbl(x) -> x * 2\nsaw(dbl(220)) |> out(@)");
    REQUIRE(r.success);
    CHECK(r.bytecode.size() > 0);
}

TEST_CASE("#inline const fn compiles", "[fn_annotations][inline]") {
    auto r = akkado::compile("#inline const fn dbl(x) -> x * 2\nsaw(dbl(220)) |> out(@)");
    REQUIRE(r.success);
}

TEST_CASE("#inline fn and plain fn emit byte-identical bytecode",
          "[fn_annotations][inline]") {
    // The compile-time-expansion model: #inline selects the codegen path but
    // produces the same instructions as the default shared-block path.
    auto plain  = akkado::compile("fn dbl(x) -> x * 2\nsaw(dbl(220)) |> out(@)");
    auto inlined = akkado::compile("#inline fn dbl(x) -> x * 2\nsaw(dbl(220)) |> out(@)");
    REQUIRE(plain.success);
    REQUIRE(inlined.success);
    REQUIRE(plain.bytecode.size() == inlined.bytecode.size());
    CHECK(std::memcmp(plain.bytecode.data(), inlined.bytecode.data(),
                      plain.bytecode.size()) == 0);
}

TEST_CASE("E246: #inline must precede a fn declaration",
          "[fn_annotations][inline][diagnostics]") {
    auto r = akkado::compile("#inline x = 1\n");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E246"));
}

TEST_CASE("E249: unknown annotation is rejected",
          "[fn_annotations][inline][diagnostics]") {
    auto r = akkado::compile("#bogus fn g(x) -> x\ng(440) |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E249"));
}

TEST_CASE("E249: # without an annotation name is rejected",
          "[fn_annotations][inline][diagnostics]") {
    auto r = akkado::compile("# = 1\n");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E249"));
}

// =============================================================================
// Recursion rejection
// =============================================================================

TEST_CASE("E240: direct self-recursion is rejected",
          "[fn_annotations][recursion][diagnostics]") {
    auto r = akkado::compile("fn f(x) -> f(x)\nf(440) |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E240"));
}

TEST_CASE("E240: mutual recursion is rejected",
          "[fn_annotations][recursion][diagnostics]") {
    auto r = akkado::compile(
        "fn a(x) -> b(x)\nfn b(x) -> a(x)\na(440) |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E240"));
}

TEST_CASE("E244: recursive #inline fn is rejected",
          "[fn_annotations][recursion][diagnostics]") {
    auto r = akkado::compile("#inline fn r(x) -> r(x)\nr(440) |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E244"));
}

TEST_CASE("E240: recursion through a pipe is rejected",
          "[fn_annotations][recursion][diagnostics]") {
    // After pipe rewriting `x |> f(@)` becomes a Call to f — still a cycle.
    auto r = akkado::compile("fn f(x) -> x |> f(@)\nf(440) |> out(@)");
    CHECK_FALSE(r.success);
    CHECK(has_diag(r, "E240"));
}

// =============================================================================
// No false positives — non-recursive fn graphs compile
// =============================================================================

TEST_CASE("non-recursive fn-calls-fn compiles cleanly",
          "[fn_annotations][recursion]") {
    auto r = akkado::compile(
        "fn a(x) -> x + 1\nfn b(x) -> a(x) * 2\nsaw(b(220)) |> out(@)");
    REQUIRE(r.success);
    CHECK_FALSE(has_diag(r, "E240"));
    CHECK_FALSE(has_diag(r, "E244"));
}

TEST_CASE("a fn called from N sites inlines N times",
          "[fn_annotations][recursion]") {
    // Default `fn` still inlines per call site (compile-time expansion).
    auto r = akkado::compile(
        "fn half(x) -> x * 0.5\n"
        "saw(half(220)) + saw(half(330)) + saw(half(440)) |> out(@)");
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::MUL) == 3);
}
