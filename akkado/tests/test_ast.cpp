// Hardening PRD Phase 6 (PRD-14): Node::extra_children[] — ghost-field
// migration tests. Guards, spread sources and destructure defaults live in
// extra_children (visited by every generic traversal), not in the data
// variant. The round-trip tests drive lex → parse → analyze and assert the
// extra children survive the analyzer's clone/substitute passes with valid
// output-arena indices.

#include <catch2/catch_test_macros.hpp>

#include "akkado/ast.hpp"
#include "akkado/analyzer.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/destructure_field.hpp"
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"

using namespace akkado;

namespace {

struct Analyzed {
    CompileContext ctx;
    AnalysisResult analysis;
};

Analyzed analyze_ok(std::string_view source) {
    Analyzed out;
    auto [tokens, lex_diags] = lex(source, *out.ctx.interner, "<test>");
    REQUIRE_FALSE(has_errors(lex_diags));
    auto [ast, parse_diags] = parse(std::move(tokens), source,
                                    *out.ctx.interner, "<test>");
    REQUIRE_FALSE(has_errors(parse_diags));
    SemanticAnalyzer analyzer(*out.ctx.interner);
    out.analysis = analyzer.analyze(ast, "<test>");
    REQUIRE(out.analysis.success);
    return out;
}

// Find the first node of `type` in the arena (arena scan, no traversal).
NodeIndex find_node(const AstArena& arena, NodeType type,
                    NodeIndex start = 0) {
    for (NodeIndex i = start; i < arena.size(); ++i) {
        if (arena[i].type == type) return i;
    }
    return NULL_NODE;
}

}  // namespace

TEST_CASE("Node::extra_child accessor", "[ast][P6]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};
    NodeIndex idx = arena.alloc(NodeType::Argument, loc);

    SECTION("empty extra_children yields NULL_NODE at any slot") {
        CHECK(arena[idx].extra_child(0) == NULL_NODE);
        CHECK(arena[idx].extra_child(7) == NULL_NODE);
    }

    SECTION("populated slots round-trip, out-of-range stays NULL_NODE") {
        NodeIndex payload = arena.alloc(NodeType::NumberLit, loc);
        arena[idx].extra_children.push_back(payload);
        CHECK(arena[idx].extra_child(0) == payload);
        CHECK(arena[idx].extra_child(1) == NULL_NODE);
    }

    SECTION("NULL_NODE placeholder entries read back as NULL_NODE") {
        arena[idx].extra_children.assign(3, NULL_NODE);
        NodeIndex payload = arena.alloc(NodeType::NumberLit, loc);
        arena[idx].extra_children[1] = payload;
        CHECK(arena[idx].extra_child(0) == NULL_NODE);
        CHECK(arena[idx].extra_child(1) == payload);
        CHECK(arena[idx].extra_child(2) == NULL_NODE);
    }
}

TEST_CASE("extra_child_kinds slot names", "[ast][P6]") {
    CHECK(extra_child_kinds(NodeType::MatchArm).size() == 1);
    CHECK(std::string_view(extra_child_kinds(NodeType::MatchArm)[0]) == "guard");
    CHECK(std::string_view(extra_child_kinds(NodeType::Argument)[0]) == "spread");
    CHECK(std::string_view(extra_child_kinds(NodeType::RecordLit)[0]) == "spread");
    CHECK(std::string_view(extra_child_kinds(NodeType::DestructureAssignment)[0])
          == "default");
    CHECK(std::string_view(extra_child_kinds(NodeType::DestructureParam)[0])
          == "default");
    CHECK(extra_child_kinds(NodeType::Call).empty());
    CHECK(extra_child_kinds(NodeType::NumberLit).empty());
}

TEST_CASE("match-arm guard survives the analyzer clone", "[ast][P6]") {
    auto a = analyze_ok(R"(
        x = 1
        y = match(x) {
            1 && x > 0: 10
            _: 0
        }
    )");
    const AstArena& arena = a.analysis.transformed_ast.arena;

    NodeIndex arm = find_node(arena, NodeType::MatchArm);
    REQUIRE(arm != NULL_NODE);
    const auto& arm_data = arena[arm].as_match_arm();
    CHECK(arm_data.has_guard);

    NodeIndex guard = arena[arm].extra_child(0);
    REQUIRE(guard != NULL_NODE);
    REQUIRE(arena.valid(guard));
    // `x > 0` desugars to gt(x, 0) — a Call in the cloned output arena.
    CHECK(arena[guard].type == NodeType::Call);
}

TEST_CASE("destructure defaults survive the analyzer clone", "[ast][P6]") {
    auto a = analyze_ok(R"(
        r = {a: 1, b: 2}
        {a = 99, b} = r
    )");
    const AstArena& arena = a.analysis.transformed_ast.arena;

    NodeIndex dn = find_node(arena, NodeType::DestructureAssignment);
    REQUIRE(dn != NULL_NODE);
    const auto& dd = arena[dn].as_destructure_assignment();
    REQUIRE(dd.fields.size() == 2);
    CHECK(dd.fields[0].name == "a");
    CHECK(dd.fields[1].name == "b");

    // extra_children stays index-aligned with fields: default for `a`,
    // NULL_NODE placeholder for `b`.
    REQUIRE(arena[dn].extra_children.size() == 2);
    NodeIndex def0 = arena[dn].extra_child(0);
    REQUIRE(def0 != NULL_NODE);
    REQUIRE(arena.valid(def0));
    CHECK(arena[def0].type == NodeType::NumberLit);
    CHECK(arena[def0].as_number() == 99.0);
    CHECK(arena[dn].extra_child(1) == NULL_NODE);

    // The zipped (name, default) plumbing view matches.
    auto bindings = destructure_bindings(arena[dn]);
    REQUIRE(bindings.size() == 2);
    CHECK(bindings[0].name == "a");
    CHECK(bindings[0].default_node == def0);
    CHECK(bindings[1].default_node == NULL_NODE);
}

TEST_CASE("record spread source survives the analyzer clone", "[ast][P6]") {
    auto a = analyze_ok(R"(
        base = {freq: 440}
        q = {..base, vel: 1}
    )");
    const AstArena& arena = a.analysis.transformed_ast.arena;

    // Find the RecordLit that carries a spread extra child.
    NodeIndex rec = NULL_NODE;
    for (NodeIndex i = 0; i < arena.size(); ++i) {
        if (arena[i].type == NodeType::RecordLit &&
            arena[i].extra_child(0) != NULL_NODE) {
            rec = i;
            break;
        }
    }
    REQUIRE(rec != NULL_NODE);
    NodeIndex spread = arena[rec].extra_child(0);
    REQUIRE(arena.valid(spread));
    CHECK(arena[spread].type == NodeType::Identifier);
}

TEST_CASE("argument spread source survives the analyzer clone", "[ast][P6]") {
    auto a = analyze_ok(R"(
        fn f(a, b) -> a + b
        r = {a: 1, b: 2}
        z = f(..r)
    )");
    const AstArena& arena = a.analysis.transformed_ast.arena;

    NodeIndex arg = NULL_NODE;
    for (NodeIndex i = 0; i < arena.size(); ++i) {
        if (arena[i].type == NodeType::Argument &&
            arena[i].extra_child(0) != NULL_NODE) {
            arg = i;
            break;
        }
    }
    REQUIRE(arg != NULL_NODE);
    NodeIndex spread = arena[arg].extra_child(0);
    REQUIRE(arena.valid(spread));
    CHECK(arena[spread].type == NodeType::Identifier);
    // Spread expression hangs only off extra_children, never as a child.
    CHECK(arena[arg].first_child == NULL_NODE);
}

TEST_CASE("destructure-param defaults survive the analyzer clone",
          "[ast][P6]") {
    auto a = analyze_ok(R"(
        fn synth({freq = 440, wave}) -> freq
        y = synth({freq: 100, wave: 1})
    )");
    const AstArena& arena = a.analysis.transformed_ast.arena;

    NodeIndex dp = find_node(arena, NodeType::DestructureParam);
    REQUIRE(dp != NULL_NODE);
    const auto& dpd = arena[dp].as_destructure_param();
    REQUIRE(dpd.fields.size() == 2);
    REQUIRE(arena[dp].extra_children.size() == 2);
    NodeIndex def0 = arena[dp].extra_child(0);
    REQUIRE(def0 != NULL_NODE);
    REQUIRE(arena.valid(def0));
    CHECK(arena[def0].type == NodeType::NumberLit);
    CHECK(arena[dp].extra_child(1) == NULL_NODE);
}
