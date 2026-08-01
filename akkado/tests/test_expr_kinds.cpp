// Hardening PRD Phase 7 (PRD-13): expr_kinds — single source of truth
// for expression-kind predicates and MIDI→Hz.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "akkado/analyzer.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/expr_kinds.hpp"
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"

using namespace akkado;
using Catch::Approx;

namespace {

struct Parsed {
    CompileContext ctx;
    Ast ast;
};

Parsed parse_ok(std::string_view source) {
    Parsed out;
    auto [tokens, lex_diags] = lex(source, *out.ctx.interner, "<test>");
    REQUIRE_FALSE(has_errors(lex_diags));
    auto [ast, parse_diags] = parse(std::move(tokens), source,
                                    *out.ctx.interner, "<test>");
    REQUIRE_FALSE(has_errors(parse_diags));
    out.ast = std::move(ast);
    return out;
}

// RHS of the last top-level statement (Assignment child).
NodeIndex last_stmt_rhs(const Ast& ast) {
    NodeIndex stmt = ast.arena[ast.root].first_child;
    while (ast.arena[stmt].next_sibling != NULL_NODE) {
        stmt = ast.arena[stmt].next_sibling;
    }
    return ast.arena[stmt].first_child;
}

}  // namespace

TEST_CASE("midi_to_hz single source of truth", "[expr_kinds][P7]") {
    CHECK(expr_kinds::midi_to_hz(69.0) == Approx(440.0));
    CHECK(expr_kinds::midi_to_hz(81.0) == Approx(880.0));
    CHECK(expr_kinds::midi_to_hz(57.0) == Approx(220.0));
    CHECK(expr_kinds::midi_to_hz(60.0) == Approx(261.6255653));
}

TEST_CASE("is_pattern_producing_call_name canonical list", "[expr_kinds][P7]") {
    CHECK(expr_kinds::is_pattern_producing_call_name("chord"));
    CHECK(expr_kinds::is_pattern_producing_call_name("timeline"));
    CHECK(expr_kinds::is_pattern_producing_call_name("seq"));
    CHECK_FALSE(expr_kinds::is_pattern_producing_call_name("sample"));  // SAMPLE_PLAY player
    CHECK_FALSE(expr_kinds::is_pattern_producing_call_name("pat"));     // removed 2026-05-20
    CHECK_FALSE(expr_kinds::is_pattern_producing_call_name("saw"));
    CHECK_FALSE(expr_kinds::is_pattern_producing_call_name(""));
}

TEST_CASE("is_literal_value", "[expr_kinds][P7]") {
    auto p = parse_ok(R"(
        a = 1
        b = true
        c = "hi"
        d = 'c4'
        e = [1, 2]
    )");
    NodeIndex stmt = p.ast.arena[p.ast.root].first_child;
    std::vector<bool> expected = {true, true, true, true, false};
    for (bool exp : expected) {
        REQUIRE(stmt != NULL_NODE);
        NodeIndex rhs = p.ast.arena[stmt].first_child;
        CHECK(expr_kinds::is_literal_value(p.ast.arena[rhs]) == exp);
        stmt = p.ast.arena[stmt].next_sibling;
    }
}

TEST_CASE("is_pattern_producer structural forms", "[expr_kinds][P7]") {
    SECTION("mini literal") {
        auto p = parse_ok(R"(x = n"c4 e4")");
        CHECK(expr_kinds::is_pattern_producer(p.ast.arena, last_stmt_rhs(p.ast),
                                              *p.ctx.interner));
    }
    SECTION("chord call") {
        auto p = parse_ok(R"(x = chord("Am F"))");
        CHECK(expr_kinds::is_pattern_producer(p.ast.arena, last_stmt_rhs(p.ast),
                                              *p.ctx.interner));
    }
    SECTION("method chain rooted at a mini literal") {
        auto p = parse_ok(R"(x = n"c4 e4".slow(2).rev())");
        CHECK(expr_kinds::is_pattern_producer(p.ast.arena, last_stmt_rhs(p.ast),
                                              *p.ctx.interner));
    }
    SECTION("plain call is not a producer") {
        auto p = parse_ok("x = saw(220)");
        CHECK_FALSE(expr_kinds::is_pattern_producer(
            p.ast.arena, last_stmt_rhs(p.ast), *p.ctx.interner));
    }
    SECTION("number literal is not a producer") {
        auto p = parse_ok("x = 42");
        CHECK_FALSE(expr_kinds::is_pattern_producer(
            p.ast.arena, last_stmt_rhs(p.ast), *p.ctx.interner));
    }
}

TEST_CASE("predicate agrees with the analyzer's Pattern binding",
          "[expr_kinds][P7]") {
    // Round-trip per PRD exit criteria: a fixture whose RHS the predicate
    // recognises must be bound as SymbolKind::Pattern by the analyzer.
    auto p = parse_ok(R"(x = c"Am F C G")");
    NodeIndex rhs = last_stmt_rhs(p.ast);
    REQUIRE(expr_kinds::is_pattern_producer(p.ast.arena, rhs,
                                            *p.ctx.interner));

    SemanticAnalyzer analyzer(*p.ctx.interner);
    auto analysis = analyzer.analyze(p.ast, "<test>");
    REQUIRE(analysis.success);
    auto sym = analysis.symbols.lookup("x");
    REQUIRE(sym.has_value());
    CHECK(sym->kind == SymbolKind::Pattern);
}

TEST_CASE("try_const_value conservative resolve", "[expr_kinds][P7]") {
    auto p = parse_ok(R"(
        a = 42
        b = true
        c = 'a4'
        d = [1, 2, 3]
        e = saw(220)
    )");
    SymbolTable symbols;  // empty — identifier lookups resolve to nothing

    NodeIndex stmt = p.ast.arena[p.ast.root].first_child;
    // a = 42
    auto v = expr_kinds::try_const_value(p.ast.arena,
                                         p.ast.arena[stmt].first_child, symbols);
    REQUIRE(v.has_value());
    CHECK(std::get<double>(*v) == 42.0);
    CHECK(expr_kinds::is_const_evaluable(p.ast.arena,
                                         p.ast.arena[stmt].first_child, symbols));
    // b = true
    stmt = p.ast.arena[stmt].next_sibling;
    v = expr_kinds::try_const_value(p.ast.arena, p.ast.arena[stmt].first_child,
                                    symbols);
    REQUIRE(v.has_value());
    CHECK(std::get<double>(*v) == 1.0);
    // c = 'a4' — pitch literal resolves through midi_to_hz
    stmt = p.ast.arena[stmt].next_sibling;
    v = expr_kinds::try_const_value(p.ast.arena, p.ast.arena[stmt].first_child,
                                    symbols);
    REQUIRE(v.has_value());
    CHECK(std::get<double>(*v) == Approx(440.0));
    // d = [1, 2, 3]
    stmt = p.ast.arena[stmt].next_sibling;
    v = expr_kinds::try_const_value(p.ast.arena, p.ast.arena[stmt].first_child,
                                    symbols);
    REQUIRE(v.has_value());
    REQUIRE(std::holds_alternative<std::vector<double>>(*v));
    CHECK(std::get<std::vector<double>>(*v).size() == 3);
    // e = saw(220) — a call is not trivially const
    stmt = p.ast.arena[stmt].next_sibling;
    CHECK_FALSE(expr_kinds::try_const_value(
        p.ast.arena, p.ast.arena[stmt].first_child, symbols).has_value());
    CHECK_FALSE(expr_kinds::is_const_evaluable(
        p.ast.arena, p.ast.arena[stmt].first_child, symbols));
}
