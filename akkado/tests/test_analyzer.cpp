#include <catch2/catch_test_macros.hpp>
#include <akkado/akkado.hpp>
#include <akkado/analyzer.hpp>
#include <akkado/parser.hpp>
#include <string>

using namespace akkado;

// Helper to check for specific error code in diagnostics
static bool has_error(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& d : diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// =============================================================================
// Analyzer: Variable immutability (E150)
// =============================================================================

TEST_CASE("Analyzer: Variable reassignment errors", "[analyzer][errors]") {
    SECTION("reassign variable in same scope - E150") {
        auto result = compile("x = 1\nx = 2");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E150"));
    }

    SECTION("reassign array - E150") {
        auto result = compile("arr = [1, 2]\narr = [3, 4]");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E150"));
    }

    SECTION("reassign record - E150") {
        auto result = compile("r = {x: 1}\nr = {y: 2}");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E150"));
    }

    SECTION("reassign pattern - E150") {
        auto result = compile("p = pat(\"c4\")\np = pat(\"e4\")");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E150"));
    }

    SECTION("reassign lambda - E150") {
        auto result = compile("f = (x) -> x * 2\nf = (x) -> x + 1");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E150"));
    }
}

// =============================================================================
// Analyzer: Named argument parsing
// =============================================================================

TEST_CASE("Analyzer: Named argument parsing", "[analyzer]") {
    SECTION("named arguments with colon syntax") {
        auto result = compile("osc(type: \"sin\", freq: 440)");
        CHECK(result.success);
    }

    SECTION("mixed positional and named arguments") {
        auto result = compile("osc(\"sin\", freq: 440)");
        CHECK(result.success);
    }

    SECTION("all named arguments") {
        auto result = compile("param(name: \"test\", default: 0.5, min: 0, max: 1)");
        CHECK(result.success);
    }
}

// =============================================================================
// Analyzer: Hole outside pipe (E003)
// =============================================================================

TEST_CASE("Analyzer: Hole errors", "[analyzer][errors]") {
    SECTION("hole in function call outside pipe - E003") {
        auto result = compile("sin(%, 440)");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E003"));
    }

    SECTION("hole in binary expression outside pipe - E003") {
        auto result = compile("x = % + 1");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E003"));
    }
}

// =============================================================================
// Analyzer: Unknown function (E004)
// =============================================================================

TEST_CASE("Analyzer: Function errors", "[analyzer][errors]") {
    SECTION("call unknown function - E004") {
        auto result = compile("totally_unknown_function(1, 2, 3)");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E004"));
    }

    SECTION("call osc with too few arguments - E006") {
        auto result = compile("osc(\"sin\")");  // osc requires waveform + freq
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E006"));
    }
}

// =============================================================================
// Analyzer: Field access errors (E060, E061)
// =============================================================================

TEST_CASE("Analyzer: Field access errors", "[analyzer][errors]") {
    SECTION("unknown field on record - E060") {
        auto result = compile("rec = {a: 1, b: 2}\nrec.nonexistent");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E060"));
    }

    SECTION("field access on scalar - E061") {
        auto result = compile("num = 42\nnum.field");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E061"));
    }

    SECTION("field access on array - E061") {
        auto result = compile("arr = [1, 2, 3]\narr.length");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E061"));
    }
}

// =============================================================================
// Analyzer: Undefined identifier (E005)
// =============================================================================

TEST_CASE("Analyzer: Undefined identifier errors", "[analyzer][errors]") {
    SECTION("use undefined variable - E005") {
        auto result = compile("y = x + 1");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E005"));
    }

    SECTION("use undefined in array - E005") {
        auto result = compile("[1, undefined, 3]");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E005"));
    }

    SECTION("use undefined in record field - E005") {
        auto result = compile("{x: undefined_value}");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E005"));
    }
}

// =============================================================================
// Analyzer: Scope resolution
// =============================================================================

TEST_CASE("Analyzer: Scope resolution", "[analyzer]") {
    SECTION("variable defined before use succeeds") {
        auto result = compile("x = 10\ny = x * 2");
        CHECK(result.success);
    }

    SECTION("user function reference succeeds") {
        auto result = compile("fn double(x) -> x * 2\ny = double(5)");
        CHECK(result.success);
    }

    SECTION("lambda as value succeeds") {
        auto result = compile("f = (x) -> x * 2\ny = map([1, 2, 3], f)");
        CHECK(result.success);
    }

    SECTION("pattern variable succeeds") {
        auto result = compile("p = pat(\"c4 e4 g4\")\np");
        CHECK(result.success);
    }

    SECTION("array variable succeeds") {
        auto result = compile("arr = [1, 2, 3]\nsum(arr)");
        CHECK(result.success);
    }
}

// =============================================================================
// Analyzer: Pipe rewriting
// =============================================================================

TEST_CASE("Analyzer: Pipe transformations", "[analyzer]") {
    SECTION("simple pipe rewrite") {
        auto result = compile("osc(\"sin\", 440) |> out(%, %)");
        CHECK(result.success);
    }

    SECTION("pipe with pattern") {
        auto result = compile("pat(\"c4 e4 g4\") |> osc(\"sin\", %.freq)");
        CHECK(result.success);
    }

    SECTION("pipe binding with as") {
        auto result = compile("osc(\"sin\", 440) as sig |> out(sig, sig)");
        CHECK(result.success);
    }

    SECTION("multiple holes in pipe stage") {
        auto result = compile("osc(\"sin\", 440) |> out(%, %)");
        CHECK(result.success);
    }
}

// =============================================================================
// Analyzer: User function definitions
// =============================================================================

TEST_CASE("Analyzer: User function handling", "[analyzer]") {
    SECTION("function with default parameter value") {
        auto result = compile("fn scale(x, factor = 2) -> x * factor\nscale(5)");
        CHECK(result.success);
    }

    SECTION("function call with explicit parameter") {
        auto result = compile("fn scale(x, factor = 2) -> x * factor\nscale(5, 10)");
        CHECK(result.success);
    }

    SECTION("function with multiple parameters") {
        auto result = compile("fn add3(a, b, c) -> a + b + c\nadd3(1, 2, 3)");
        CHECK(result.success);
    }

    SECTION("nested function calls") {
        auto result = compile("fn double(x) -> x * 2\nfn quad(x) -> double(double(x))\nquad(3)");
        CHECK(result.success);
    }
}

// =============================================================================
// Analyzer: Match expressions
// =============================================================================

TEST_CASE("Analyzer: Match expression analysis", "[analyzer]") {
    SECTION("simple match expression") {
        auto result = compile("match(\"sin\") { \"sin\": 1, \"saw\": 2, _: 0 }");
        CHECK(result.success);
    }

    SECTION("match with number patterns") {
        auto result = compile("match(5) { 0: 0, 1: 1, _: 99 }");
        CHECK(result.success);
    }
}
