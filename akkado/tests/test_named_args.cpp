// Hardening PRD Phase 7 (PRD-13): canonical named-argument slot
// resolver, exercised against all three schema variants the former
// mirrors used (BuiltinInfo-like find_param, param-name vector, and the
// codegen spread path's warn-and-drop mode).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include "akkado/named_args.hpp"

using namespace akkado;

namespace {

SourceLocation loc(std::uint32_t line) {
    return SourceLocation{line, 1, 0, 0};
}

// Vector-schema find_param (the user-function mirror).
std::function<int(std::string_view, std::size_t)>
name_vector_schema(std::vector<std::string> names) {
    return [names = std::move(names)](std::string_view name, std::size_t) {
        for (std::size_t j = 0; j < names.size(); ++j) {
            if (names[j] == name) return static_cast<int>(j);
        }
        return -1;
    };
}

}  // namespace

TEST_CASE("all-positional calls need no reorder", "[named_args][P7]") {
    std::vector<NamedArgInput> args = {{std::nullopt, loc(1)},
                                       {std::nullopt, loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a", "b"}), false);
    CHECK(r.ok);
    CHECK_FALSE(r.has_named);
    CHECK(r.slot_source.empty());
}

TEST_CASE("named args map to declared slots with gap-fill markers",
          "[named_args][P7]") {
    // f(x, c: 3) against params (a, b, c): slot 0 = arg0, slot 1 = gap,
    // slot 2 = arg1.
    std::vector<NamedArgInput> args = {{std::nullopt, loc(1)},
                                       {std::string("c"), loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a", "b", "c"}), false);
    REQUIRE(r.ok);
    REQUIRE(r.has_named);
    REQUIRE(r.slot_source.size() == 3);
    CHECK(r.slot_source[0] == 0);
    CHECK(r.slot_source[1] == -1);
    CHECK(r.slot_source[2] == 1);
}

TEST_CASE("named args reorder out-of-order names", "[named_args][P7]") {
    // f(b: 2, a: 1) → slot 0 = arg1, slot 1 = arg0.
    std::vector<NamedArgInput> args = {{std::string("b"), loc(1)},
                                       {std::string("a"), loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a", "b"}), false);
    REQUIRE(r.ok);
    REQUIRE(r.slot_source.size() == 2);
    CHECK(r.slot_source[0] == 1);
    CHECK(r.slot_source[1] == 0);
}

TEST_CASE("duplicate named argument is E010", "[named_args][P7]") {
    std::vector<NamedArgInput> args = {{std::string("a"), loc(1)},
                                       {std::string("a"), loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a", "b"}), false);
    CHECK_FALSE(r.ok);
    CHECK(std::string_view(r.code) == "E010");
    CHECK(r.error_loc.line == 2);
}

TEST_CASE("unknown parameter is E011 in strict mode", "[named_args][P7]") {
    std::vector<NamedArgInput> args = {{std::string("nope"), loc(3)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a"}), false);
    CHECK_FALSE(r.ok);
    CHECK(std::string_view(r.code) == "E011");
    CHECK(r.error_loc.line == 3);
}

TEST_CASE("unknown parameter is dropped in drop_unknown mode",
          "[named_args][P7]") {
    // The codegen spread mirror: unknown fields warn (caller-side W160)
    // and drop; known fields still land in their slots.
    std::vector<NamedArgInput> args = {{std::string("nope"), loc(1)},
                                       {std::string("b"), loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a", "b"}), true);
    REQUIRE(r.ok);
    REQUIRE(r.dropped.size() == 1);
    CHECK(r.dropped[0] == 0);
    REQUIRE(r.slot_source.size() == 2);
    CHECK(r.slot_source[0] == -1);
    CHECK(r.slot_source[1] == 1);
}

TEST_CASE("every argument dropped yields empty slot table",
          "[named_args][P7]") {
    std::vector<NamedArgInput> args = {{std::string("x"), loc(1)},
                                       {std::string("y"), loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a"}), true);
    REQUIRE(r.ok);
    CHECK(r.has_named);
    CHECK(r.slot_source.empty());
    CHECK(r.dropped.size() == 2);
}

TEST_CASE("positional after named is E009", "[named_args][P7]") {
    std::vector<NamedArgInput> args = {{std::string("b"), loc(1)},
                                       {std::nullopt, loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a", "b"}), false);
    CHECK_FALSE(r.ok);
    CHECK(std::string_view(r.code) == "E009");
    CHECK(r.error_loc.line == 2);
}

TEST_CASE("named/positional slot collision is E012", "[named_args][P7]") {
    // f(1, a: 2): positional fills slot 0, a: also maps to slot 0.
    std::vector<NamedArgInput> args = {{std::nullopt, loc(1)},
                                       {std::string("a"), loc(2)}};
    auto r = assign_named_arg_slots(args, "f",
                                    name_vector_schema({"a", "b"}), false);
    CHECK_FALSE(r.ok);
    CHECK(std::string_view(r.code) == "E012");
    CHECK(r.error_loc.line == 1);  // anchored at the positional arg
}

TEST_CASE("kNamedArgAbort aborts without a core diagnostic",
          "[named_args][P7]") {
    // The analyzer's open-kwarg overflow (E263) path: the callback emits
    // its own error and returns the abort sentinel.
    std::vector<NamedArgInput> args = {{std::string("custom"), loc(1)}};
    auto r = assign_named_arg_slots(
        args, "f",
        [](std::string_view, std::size_t) { return kNamedArgAbort; }, false);
    CHECK_FALSE(r.ok);
    CHECK(r.code == nullptr);
}

TEST_CASE("BuiltinInfo-style extended index space passes through",
          "[named_args][P7]") {
    // Mimics find_param mapping an extended param past the regular slots
    // (regular a,b then extended e0 at index 5) — slot table grows to fit.
    auto schema = [](std::string_view name, std::size_t) -> int {
        if (name == "a") return 0;
        if (name == "b") return 1;
        if (name == "e0") return 5;
        return -1;
    };
    std::vector<NamedArgInput> args = {{std::nullopt, loc(1)},
                                       {std::string("e0"), loc(2)}};
    auto r = assign_named_arg_slots(args, "f", schema, false);
    REQUIRE(r.ok);
    REQUIRE(r.slot_source.size() == 6);
    CHECK(r.slot_source[0] == 0);
    CHECK(r.slot_source[5] == 1);
    for (std::size_t i = 1; i < 5; ++i) CHECK(r.slot_source[i] == -1);
}
