// UFCS method-call codegen tests.
//
// Verifies that x.foo(a, b) lowers identically to foo(x, a, b) for any
// callable in scope (builtins and user closures alike). This is the Phase 2
// payload from prd-userspace-state-and-edge-primitives.md.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <vector>

static std::vector<cedar::Instruction> get_instructions(
    const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> insts;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    insts.resize(count);
    std::memcpy(insts.data(), result.bytecode.data(), result.bytecode.size());
    return insts;
}

// Strip transient fields (state_id, out_buffer indices that depend on
// allocation order) for structural comparison. We compare the opcode + rate
// pattern; identical opcode order with identical rate values means the two
// programs do the same work even if buffer indices differ.
static std::vector<std::pair<cedar::Opcode, std::uint8_t>> opcode_pattern(
    const std::vector<cedar::Instruction>& insts) {
    std::vector<std::pair<cedar::Opcode, std::uint8_t>> p;
    p.reserve(insts.size());
    for (const auto& i : insts) p.emplace_back(i.opcode, i.rate);
    return p;
}

TEST_CASE("UFCS: builtin method call lowers to function call", "[methods][ufcs]") {
    // Both programs should produce structurally identical bytecode.
    auto method_form = akkado::compile(
        "sg = sine(440)\n"
        "sg.lp(1200) |> out(%, %)\n"
    );
    auto function_form = akkado::compile(
        "sg = sine(440)\n"
        "lp(sg, 1200) |> out(%, %)\n"
    );

    REQUIRE(method_form.success);
    REQUIRE(function_form.success);

    auto a = opcode_pattern(get_instructions(method_form));
    auto b = opcode_pattern(get_instructions(function_form));
    CHECK(a == b);
}

TEST_CASE("UFCS: chained method calls", "[methods][ufcs]") {
    // sg.lp(1200).hp(200) ≡ hp(lp(sg, 1200), 200)
    auto chain = akkado::compile(
        "sg = sine(440)\n"
        "sg.lp(1200).hp(200) |> out(%, %)\n"
    );
    auto nested = akkado::compile(
        "sg = sine(440)\n"
        "hp(lp(sg, 1200), 200) |> out(%, %)\n"
    );

    REQUIRE(chain.success);
    REQUIRE(nested.success);
    CHECK(opcode_pattern(get_instructions(chain)) ==
          opcode_pattern(get_instructions(nested)));
}

TEST_CASE("UFCS: mixed pipe and method", "[methods][ufcs]") {
    // sg.lp(1200) |> hp(%, 200) — both forms cooperate.
    auto mixed = akkado::compile(
        "sg = sine(440)\n"
        "sg.lp(1200) |> hp(%, 200) |> out(%, %)\n"
    );
    REQUIRE(mixed.success);
    // Should compile without UFCS-vs-pipe interaction errors.
    auto insts = get_instructions(mixed);
    bool has_lp = false, has_hp = false;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::FILTER_SVF_LP) has_lp = true;
        if (i.opcode == cedar::Opcode::FILTER_SVF_HP) has_hp = true;
    }
    CHECK(has_lp);
    CHECK(has_hp);
}

TEST_CASE("UFCS: user closure called as method", "[methods][ufcs]") {
    // A user-defined closure invoked via UFCS should behave the same as the
    // direct call form.
    auto method = akkado::compile(
        "double = (x) -> x * 2\n"
        "sg = sine(440)\n"
        "sg.double() |> out(%, %)\n"
    );
    auto direct = akkado::compile(
        "double = (x) -> x * 2\n"
        "sg = sine(440)\n"
        "double(sg) |> out(%, %)\n"
    );
    REQUIRE(method.success);
    REQUIRE(direct.success);
    CHECK(opcode_pattern(get_instructions(method)) ==
          opcode_pattern(get_instructions(direct)));
}

TEST_CASE("UFCS: undefined method name produces helpful error", "[methods][ufcs]") {
    auto result = akkado::compile(
        "sg = sine(440)\n"
        "sg.no_such_op(1) |> out(%, %)\n"
    );
    REQUIRE_FALSE(result.success);
    // Error should reference the name so the user knows what's missing.
    bool found_name = false;
    for (const auto& d : result.diagnostics) {
        if (d.message.find("no_such_op") != std::string::npos) {
            found_name = true;
            break;
        }
    }
    CHECK(found_name);
}
