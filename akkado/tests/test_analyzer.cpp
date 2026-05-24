#include <catch2/catch_test_macros.hpp>
#include <akkado/akkado.hpp>
#include <akkado/analyzer.hpp>
#include <akkado/lexer.hpp>
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
        // `arr` is now reserved (Phase 2). Use `xs` instead.
        auto result = compile("xs = [1, 2]\nxs = [3, 4]");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E150"));
    }

    SECTION("reassign record - E150") {
        auto result = compile("r = {x: 1}\nr = {y: 2}");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E150"));
    }

    SECTION("reassign pattern - E150") {
        auto result = compile("p = n\"c4\"\np = n\"e4\"");
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
    // `rec` / `num` / `arr` are now reserved (Phase 2). Use neutral names.
    SECTION("unknown field on record - E060") {
        auto result = compile("r = {a: 1, b: 2}\nr.nonexistent");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E060"));
    }

    SECTION("field access on scalar - E061") {
        auto result = compile("n = 42\nn.field");
        REQUIRE_FALSE(result.success);
        CHECK(has_error(result.diagnostics, "E061"));
    }

    SECTION("field access on array - E061") {
        auto result = compile("xs = [1, 2, 3]\nxs.length");
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
        auto result = compile("p = n\"c4 e4 g4\"\np");
        CHECK(result.success);
    }

    SECTION("array variable succeeds") {
        auto result = compile("xs = [1, 2, 3]\nsum(xs)");
        CHECK(result.success);
    }

    // PRD prd-remove-pat-builtin §6.2: `pat` and `p` are ordinary identifiers
    // after the builtin removal.
    SECTION("pat is usable as an ordinary identifier") {
        auto result = compile("pat = n\"c4 e4\"\npat |> osc(\"sin\", @.freq) |> out(@, @)");
        CHECK(result.success);
    }

    SECTION("p is usable as an ordinary identifier") {
        auto result = compile("p = osc(\"sin\", 440)\nout(p, p)");
        CHECK(result.success);
    }

    // PRD prd-remove-pat-builtin §10.1: the removed builtins error generically.
    SECTION("n'…' call errors as unknown builtin") {
        auto result = compile("pat(\"c4 e4\") |> out(@, @)");
        CHECK_FALSE(result.success);
    }

    SECTION("value() call errors as unknown builtin") {
        auto result = compile("value(\"0.5\") |> out(@, @)");
        CHECK_FALSE(result.success);
    }

    SECTION("note() call errors as unknown builtin") {
        auto result = compile("note(\"c4 e4\") |> out(@, @)");
        CHECK_FALSE(result.success);
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
        auto result = compile("n\"c4 e4 g4\" |> osc(\"sin\", %.freq)");
        CHECK(result.success);
    }

    SECTION("pipe binding with as") {
        auto result = compile("osc(\"sin\", 440) as s |> out(s, s)");
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

// =============================================================================
// Analyzer: Spread arguments — Phase 2 validation
// =============================================================================

// Helper: run lexer + parser + analyzer only (no codegen).
// Codegen for spread args is implemented in later phases; analyzer-only tests
// verify that argument-count and reordering checks are deferred when spread
// is present, without exercising the still-unimplemented codegen path.
static std::vector<Diagnostic> analyze_only(std::string_view src) {
    auto [tokens, lex_diags] = lex(src);
    auto [ast, parse_diags] = parse(std::move(tokens), src);
    SemanticAnalyzer analyzer;
    auto result = analyzer.analyze(ast, "<input>");
    return result.diagnostics;
}

TEST_CASE("Analyzer: Spread arg defers count check", "[analyzer][spread]") {
    SECTION("user fn with too many args is rejected without spread") {
        auto diags = analyze_only(
            "fn f(a, b) -> a + b\n"
            "f(1, 2, 3)");
        CHECK(has_error(diags, "E007"));
    }

    SECTION("user fn with spread skips E007 max-arg check") {
        // r could expand to fewer or more than 'too many' fields — analyzer
        // must NOT statically reject. Codegen handles the actual binding.
        auto diags = analyze_only(
            "fn f(a, b) -> a + b\n"
            "r = {a: 1, b: 2}\n"
            "f(..r)");
        CHECK_FALSE(has_error(diags, "E007"));
        CHECK_FALSE(has_error(diags, "E006"));
    }

    SECTION("user fn with spread skips E006 min-arg check") {
        auto diags = analyze_only(
            "fn f(a, b, c) -> a + b + c\n"
            "r = {a: 1, b: 2, c: 3}\n"
            "f(..r)");
        CHECK_FALSE(has_error(diags, "E006"));
    }

    SECTION("builtin tap_delay with spread skips E301") {
        // Without spread, tap_delay needs 4-6 args.
        auto diags = analyze_only(
            "config = {time: 0.5, fb: 0.5}\n"
            "tap_delay(osc(\"sin\", 440), ..config, (x) -> x)");
        CHECK_FALSE(has_error(diags, "E301"));
    }

    SECTION("user fn without spread still validates min-arg") {
        auto diags = analyze_only(
            "fn f(a, b, c) -> a + b + c\n"
            "f(1)");
        CHECK(has_error(diags, "E006"));
    }
}

// =============================================================================
// Field access through pattern-producer bindings (regression for the codegen
// audit follow-up — `docs/audits/mini-notation-cycle-timing_audit_2026-05-18.md`
// §10.2). The records-and-field-access PRD claims transforms preserve fields
// uniformly across every pattern producer; this block pins every row of that
// variant table.
// =============================================================================

TEST_CASE("Analyzer: field access on pattern bindings (audit §10.2)",
          "[analyzer][field-access]") {
    auto pattern_kind = [](const CompileResult& r,
                           const std::string& name) -> std::optional<SymbolKind> {
        if (!r.symbols) return std::nullopt;
        auto sym = r.symbols->lookup(name);
        if (!sym) return std::nullopt;
        return sym->kind;
    };

    SECTION("inline n\"…\" with no transform compiles") {
        auto r = compile("n\"c4 e4\" |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK_FALSE(has_error(r.diagnostics, "E124"));
    }

    SECTION("inline n\"…\".slow(2) compiles") {
        auto r = compile("n\"c4 e4\".slow(2) |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK_FALSE(has_error(r.diagnostics, "E124"));
    }

    SECTION("var bound to n\"…\" without transform — Pattern kind, field access works") {
        auto r = compile("notes = n\"c4 e4\"\nnotes |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK(pattern_kind(r, "notes") == SymbolKind::Pattern);
    }

    SECTION("var bound to n\"…\".slow(2) — Pattern kind, field access works (BUG FIX)") {
        auto r = compile("notes = n\"c4 e4\".slow(2)\nnotes |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK_FALSE(has_error(r.diagnostics, "E124"));
        CHECK(pattern_kind(r, "notes") == SymbolKind::Pattern);
    }

    SECTION("var bound to n\"…\" then transform at use — works (workaround form)") {
        auto r = compile("notes = n\"c4 e4\"\nnotes.slow(2) |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK(pattern_kind(r, "notes") == SymbolKind::Pattern);
    }

    SECTION("var bound to n\"…\".slow(2) — Pattern kind, field access works (BUG FIX)") {
        auto r = compile("notes = n\"c4 e4\".slow(2)\nnotes |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK_FALSE(has_error(r.diagnostics, "E124"));
        CHECK(pattern_kind(r, "notes") == SymbolKind::Pattern);
    }

    SECTION("var bound to n\"…\" — Pattern kind, field access works (BUG FIX)") {
        auto r = compile("notes = n\"c4 e4\"\nnotes |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK_FALSE(has_error(r.diagnostics, "E124"));
        CHECK(pattern_kind(r, "notes") == SymbolKind::Pattern);
    }

    SECTION("var bound to n\"…\".fast(2) — analyzer classifies as Pattern; codegen path is a separate bug") {
        // The analyzer's symbol classification is the contract this test
        // block exists to enforce — that part works. End-to-end compile
        // currently fails with E130 because the transform handlers
        // (handle_fast_call/handle_slow_call) don't recognize Call-to-
        // pattern-producer receivers (only MiniLiteral); inline
        // `n"…".fast(2) |> …` fails for the same reason. That's a
        // separate codegen bug; tracked as a follow-up to this fix.
        auto r = compile("notes = n\"c4 e4\".fast(2)\nnotes |> osc(\"sin\", @freq) |> out(@)");
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK(pattern_kind(r, "notes") == SymbolKind::Pattern);
    }

    SECTION("chained transforms: n\"…\".slow(2).rev() — Pattern kind, field access works") {
        auto r = compile("notes = n\"c4 e4 g4 b4\".slow(2).rev()\nnotes |> osc(\"sin\", @freq) |> out(@)");
        CHECK(r.success);
        CHECK_FALSE(has_error(r.diagnostics, "E061"));
        CHECK(pattern_kind(r, "notes") == SymbolKind::Pattern);
    }

    SECTION("field access on plain scalar variable still triggers E061") {
        // Guards against over-classification regression: non-pattern bindings
        // must still reject field access.
        auto r = compile("x = 440\nout(x.freq)");
        CHECK_FALSE(r.success);
        CHECK(has_error(r.diagnostics, "E061"));
    }
}
