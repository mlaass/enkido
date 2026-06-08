#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"
#include "akkado/string_interner.hpp"

using namespace akkado;
using Catch::Matchers::WithinRel;

namespace {
// Phase 5 (F12): the Lexer/Parser ctors now require a StringInterner.
// Tests reuse a thread_local one so existing string-literal sources
// keep their lexeme views alive for the program lifetime. Identifier
// comparisons resolve through `ident_text(id)` below.
inline StringInterner& test_interner() {
    static thread_local StringInterner i;
    return i;
}
inline auto lex(std::string_view src, std::string_view fn = "<input>") {
    return akkado::lex(src, test_interner(), fn);
}
inline auto parse(std::vector<Token> tokens, std::string_view src,
                  std::string_view fn = "<input>", bool lint = false) {
    return akkado::parse(std::move(tokens), src, test_interner(), fn, lint);
}
inline std::string_view ident_text(SymbolId id) {
    return test_interner().view(id);
}
} // namespace

// Helper to parse source and return AST
static Ast parse_source(std::string_view source) {
    auto [tokens, lex_diags] = lex(source);
    auto [ast, parse_diags] = parse(std::move(tokens), source);
    return ast;
}

// Helper to parse and check no errors
static Ast parse_ok(std::string_view source) {
    auto [tokens, lex_diags] = lex(source);
    REQUIRE(lex_diags.empty());

    auto [ast, parse_diags] = parse(std::move(tokens), source);
    if (!parse_diags.empty()) {
        for (const auto& d : parse_diags) {
            UNSCOPED_INFO("Parse error: " << d.message << " at line " << d.location.line);
        }
    }
    REQUIRE(parse_diags.empty());
    REQUIRE(ast.valid());
    return ast;
}

TEST_CASE("Parser literals", "[parser]") {
    SECTION("number literal") {
        auto ast = parse_ok("42");
        REQUIRE(ast.arena.size() >= 2);  // Program + Number

        // Program should have one child
        NodeIndex root = ast.root;
        REQUIRE(ast.arena[root].type == NodeType::Program);

        NodeIndex child = ast.arena[root].first_child;
        REQUIRE(child != NULL_NODE);
        REQUIRE(ast.arena[child].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[child].as_number(), WithinRel(42.0));
    }

    SECTION("float literal") {
        auto ast = parse_ok("3.14");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[child].as_number(), WithinRel(3.14));
    }

    SECTION("negative number") {
        auto ast = parse_ok("-1.5");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[child].as_number(), WithinRel(-1.5));
    }

    SECTION("boolean true") {
        auto ast = parse_ok("true");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::BoolLit);
        CHECK(ast.arena[child].as_bool() == true);
    }

    SECTION("boolean false") {
        auto ast = parse_ok("false");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::BoolLit);
        CHECK(ast.arena[child].as_bool() == false);
    }

    SECTION("string literal") {
        auto ast = parse_ok("\"hello world\"");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::StringLit);
        CHECK(ast.arena[child].as_string() == "hello world");
    }

    SECTION("identifier") {
        auto ast = parse_ok("foo");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "foo");
    }

    SECTION("hole") {
        auto ast = parse_ok("%");
        NodeIndex child = ast.arena[ast.root].first_child;
        CHECK(ast.arena[child].type == NodeType::Hole);
    }
}

TEST_CASE("Parser binary operators", "[parser]") {
    SECTION("addition") {
        auto ast = parse_ok("1 + 2");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "add");

        // Should have two argument children
        CHECK(ast.arena.child_count(child) == 2);
    }

    SECTION("subtraction") {
        auto ast = parse_ok("5 - 3");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "sub");
    }

    SECTION("multiplication") {
        auto ast = parse_ok("2 * 3");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "mul");
    }

    SECTION("division") {
        auto ast = parse_ok("10 / 2");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "div");
    }

    SECTION("power") {
        auto ast = parse_ok("2 ^ 3");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "pow");
    }

    SECTION("precedence: mul before add") {
        // 1 + 2 * 3 should parse as add(1, mul(2, 3))
        auto ast = parse_ok("1 + 2 * 3");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ident_text(ast.arena[expr].as_identifier()) == "add");

        // Second argument should be mul
        NodeIndex first_arg = ast.arena[expr].first_child;
        NodeIndex second_arg = ast.arena[first_arg].next_sibling;
        REQUIRE(second_arg != NULL_NODE);

        // The argument node contains the actual expression
        NodeIndex mul_expr = ast.arena[second_arg].first_child;
        REQUIRE(ast.arena[mul_expr].type == NodeType::Call);
        CHECK(ident_text(ast.arena[mul_expr].as_identifier()) == "mul");
    }

    SECTION("left associativity") {
        // 1 - 2 - 3 should parse as sub(sub(1, 2), 3)
        auto ast = parse_ok("1 - 2 - 3");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ident_text(ast.arena[expr].as_identifier()) == "sub");

        // First argument should be another sub
        NodeIndex first_arg = ast.arena[expr].first_child;
        NodeIndex inner_sub = ast.arena[first_arg].first_child;
        REQUIRE(ast.arena[inner_sub].type == NodeType::Call);
        CHECK(ident_text(ast.arena[inner_sub].as_identifier()) == "sub");
    }
}

TEST_CASE("Power right-associativity (F7 regression)", "[parser][F7]") {
    // F7 was withdrawn in Phase 0 (commit b203e2e) — `^` is already
    // right-associative on master. These tests lock current behaviour
    // so a future parser refactor can't silently regress it. See
    // docs/prd-parser-codegen-correctness.md §1.3 and §4 Phase 2.

    SECTION("2 ^ 3 ^ 2 parses as pow(2, pow(3, 2))") {
        auto ast = parse_ok("2 ^ 3 ^ 2");
        NodeIndex outer = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[outer].type == NodeType::Call);
        CHECK(ident_text(ast.arena[outer].as_identifier()) == "pow");

        NodeIndex first_arg  = ast.arena[outer].first_child;
        NodeIndex second_arg = ast.arena[first_arg].next_sibling;
        REQUIRE(second_arg != NULL_NODE);

        // First arg unwraps to NumberLit(2).
        NodeIndex first_inner = ast.arena[first_arg].first_child;
        REQUIRE(ast.arena[first_inner].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[first_inner].as_number(), WithinRel(2.0));

        // Second arg unwraps to inner pow(3, 2) — right-nested.
        NodeIndex inner = ast.arena[second_arg].first_child;
        REQUIRE(ast.arena[inner].type == NodeType::Call);
        CHECK(ident_text(ast.arena[inner].as_identifier()) == "pow");
    }

    SECTION("tower 2 ^ 2 ^ 2 ^ 2 nests right") {
        auto ast = parse_ok("2 ^ 2 ^ 2 ^ 2");
        // Walk three levels deep on the right; each level is a pow with
        // NumberLit(2) on the left.
        NodeIndex n = ast.arena[ast.root].first_child;
        for (int depth = 0; depth < 3; ++depth) {
            REQUIRE(ast.arena[n].type == NodeType::Call);
            CHECK(ident_text(ast.arena[n].as_identifier()) == "pow");
            NodeIndex left  = ast.arena[n].first_child;
            NodeIndex right = ast.arena[left].next_sibling;
            REQUIRE(right != NULL_NODE);
            NodeIndex left_inner = ast.arena[left].first_child;
            REQUIRE(ast.arena[left_inner].type == NodeType::NumberLit);
            n = ast.arena[right].first_child;
        }
        // Deepest right operand is NumberLit(2).
        REQUIRE(ast.arena[n].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[n].as_number(), WithinRel(2.0));
    }

    SECTION("-2 ^ 2 parses as pow(-2, 2) via negative-number lexing") {
        // Lexer produces a single Number(-2) token when `-` is immediately
        // followed by a digit (see lexer.cpp:246), so the AST has NumberLit(-2)
        // as the first argument — not a unary-neg call wrapping NumberLit(2).
        // Result is numerically (-2)^2 = 4 (documented divergence from
        // Python's -(2^2) = -4).
        auto ast = parse_ok("-2 ^ 2");
        NodeIndex pow_call = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pow_call].type == NodeType::Call);
        CHECK(ident_text(ast.arena[pow_call].as_identifier()) == "pow");

        NodeIndex first_arg = ast.arena[pow_call].first_child;
        NodeIndex first_inner = ast.arena[first_arg].first_child;
        REQUIRE(ast.arena[first_inner].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[first_inner].as_number(), WithinRel(-2.0));
    }

    SECTION("x ^ -1 parses cleanly") {
        // Right operand begins with `-1`, which the lexer fuses into a single
        // negative Number token. The whole expression is pow(x, -1).
        auto ast = parse_ok("x ^ -1");
        NodeIndex pow_call = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pow_call].type == NodeType::Call);
        CHECK(ident_text(ast.arena[pow_call].as_identifier()) == "pow");

        NodeIndex first_arg  = ast.arena[pow_call].first_child;
        NodeIndex second_arg = ast.arena[first_arg].next_sibling;
        REQUIRE(second_arg != NULL_NODE);
        NodeIndex second_inner = ast.arena[second_arg].first_child;
        REQUIRE(ast.arena[second_inner].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[second_inner].as_number(), WithinRel(-1.0));
    }
}

TEST_CASE("Parser function calls", "[parser]") {
    SECTION("no arguments") {
        auto ast = parse_ok("foo()");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "foo");
        CHECK(ast.arena.child_count(child) == 0);
    }

    SECTION("single argument") {
        auto ast = parse_ok("sin(440)");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "sin");
        CHECK(ast.arena.child_count(child) == 1);
    }

    SECTION("multiple arguments") {
        auto ast = parse_ok("lp(x, 1000, 0.7)");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ident_text(ast.arena[child].as_identifier()) == "lp");
        CHECK(ast.arena.child_count(child) == 3);
    }

    SECTION("named arguments") {
        auto ast = parse_ok("svflp(in: x, cut: 800, q: 0.5)");
        NodeIndex call = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);
        CHECK(ident_text(ast.arena[call].as_identifier()) == "svflp");

        // Check first argument is named
        NodeIndex first_arg = ast.arena[call].first_child;
        REQUIRE(ast.arena[first_arg].type == NodeType::Argument);
        auto& name = ast.arena[first_arg].as_arg_name();
        REQUIRE(name.has_value());
        CHECK(name.value() == "in");
    }

    SECTION("mixed positional and named") {
        auto ast = parse_ok("foo(1, 2, name: 3)");
        NodeIndex call = ast.arena[ast.root].first_child;
        CHECK(ast.arena.child_count(call) == 3);
    }

    SECTION("nested calls") {
        auto ast = parse_ok("f(g(x))");
        NodeIndex outer = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[outer].type == NodeType::Call);
        CHECK(ident_text(ast.arena[outer].as_identifier()) == "f");

        // The argument's child should be another call
        NodeIndex arg = ast.arena[outer].first_child;
        NodeIndex inner = ast.arena[arg].first_child;
        REQUIRE(ast.arena[inner].type == NodeType::Call);
        CHECK(ident_text(ast.arena[inner].as_identifier()) == "g");
    }
}

TEST_CASE("Parser pipes", "[parser]") {
    SECTION("simple pipe") {
        auto ast = parse_ok("x |> f(%)");
        NodeIndex pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);
        CHECK(ast.arena.child_count(pipe) == 2);
    }

    SECTION("pipe chain") {
        auto ast = parse_ok("a |> b(%) |> c(%)");
        NodeIndex outer_pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[outer_pipe].type == NodeType::Pipe);

        // First child should be inner pipe
        NodeIndex first = ast.arena[outer_pipe].first_child;
        REQUIRE(ast.arena[first].type == NodeType::Pipe);
    }

    SECTION("pipe with expression") {
        auto ast = parse_ok("saw(440) |> % * 0.5");
        NodeIndex pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        // Second child should be multiplication
        NodeIndex first = ast.arena[pipe].first_child;
        NodeIndex second = ast.arena[first].next_sibling;
        REQUIRE(ast.arena[second].type == NodeType::Call);
        CHECK(ident_text(ast.arena[second].as_identifier()) == "mul");
    }

    SECTION("pipe as function argument") {
        auto ast = parse_ok("f(a |> b(%))");
        NodeIndex call = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        // The argument should contain a pipe
        NodeIndex arg = ast.arena[call].first_child;
        NodeIndex pipe = ast.arena[arg].first_child;
        CHECK(ast.arena[pipe].type == NodeType::Pipe);
    }
}

TEST_CASE("Parser closures", "[parser]") {
    SECTION("empty params") {
        auto ast = parse_ok("() -> 42");
        NodeIndex closure = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[closure].type == NodeType::Closure);

        // Should have just the body (number)
        NodeIndex body = ast.arena[closure].first_child;
        CHECK(ast.arena[body].type == NodeType::NumberLit);
    }

    SECTION("single param") {
        auto ast = parse_ok("(x) -> x");
        NodeIndex closure = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[closure].type == NodeType::Closure);

        // First child is param, second is body
        NodeIndex param = ast.arena[closure].first_child;
        REQUIRE(ast.arena[param].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[param].as_identifier()) == "x");

        NodeIndex body = ast.arena[param].next_sibling;
        CHECK(ast.arena[body].type == NodeType::Identifier);
    }

    SECTION("multiple params") {
        auto ast = parse_ok("(x, y, z) -> x");
        NodeIndex closure = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[closure].type == NodeType::Closure);

        // Count children: 3 params + 1 body = 4
        std::size_t count = 0;
        NodeIndex curr = ast.arena[closure].first_child;
        while (curr != NULL_NODE) {
            count++;
            curr = ast.arena[curr].next_sibling;
        }
        CHECK(count == 4);
    }

    SECTION("closure with expression body") {
        auto ast = parse_ok("(x) -> x + 1");
        NodeIndex closure = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[closure].type == NodeType::Closure);

        // Body should be add call
        NodeIndex param = ast.arena[closure].first_child;
        NodeIndex body = ast.arena[param].next_sibling;
        REQUIRE(ast.arena[body].type == NodeType::Call);
        CHECK(ident_text(ast.arena[body].as_identifier()) == "add");
    }

    SECTION("closure with pipe in body (greedy)") {
        auto ast = parse_ok("(x) -> x |> f(%)");
        NodeIndex closure = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[closure].type == NodeType::Closure);

        // Body should be pipe (closure is greedy)
        NodeIndex param = ast.arena[closure].first_child;
        NodeIndex body = ast.arena[param].next_sibling;
        CHECK(ast.arena[body].type == NodeType::Pipe);
    }

    SECTION("closure with block body") {
        auto ast = parse_ok("(x) -> { y = x + 1\n y * 2 }");
        NodeIndex closure = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[closure].type == NodeType::Closure);

        NodeIndex param = ast.arena[closure].first_child;
        NodeIndex body = ast.arena[param].next_sibling;
        CHECK(ast.arena[body].type == NodeType::Block);
    }
}

TEST_CASE("Parser assignments", "[parser]") {
    SECTION("simple assignment") {
        auto ast = parse_ok("x = 42");
        NodeIndex assign = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[assign].type == NodeType::Assignment);
        CHECK(ident_text(ast.arena[assign].as_identifier()) == "x");

        NodeIndex value = ast.arena[assign].first_child;
        REQUIRE(ast.arena[value].type == NodeType::NumberLit);
    }

    SECTION("assignment with expression") {
        auto ast = parse_ok("bpm = 120");
        NodeIndex assign = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[assign].type == NodeType::Assignment);
        CHECK(ident_text(ast.arena[assign].as_identifier()) == "bpm");
    }

    SECTION("assignment with pipe") {
        // `sig` is now reserved (Phase 2). Use `s` as variable name instead.
        auto ast = parse_ok("s = saw(440) |> lp(%, 1000)");
        NodeIndex assign = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[assign].type == NodeType::Assignment);

        NodeIndex value = ast.arena[assign].first_child;
        CHECK(ast.arena[value].type == NodeType::Pipe);
    }
}

TEST_CASE("Parser mini-notation", "[parser]") {
    SECTION("simple pat") {
        auto ast = parse_ok("s\"bd sd\"");
        NodeIndex mini = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[mini].type == NodeType::MiniLiteral);

        // PRD Phase 1b: the parsed MiniPattern lives in MiniLiteralData's
        // sub-arena, not as a first-child of the MiniLiteral node.
        const auto& lit_data = ast.arena[mini].as_mini_literal();
        REQUIRE(lit_data.mini_arena);
        REQUIRE(lit_data.mini_root != NULL_NODE);
        const AstArena& mini_arena = *lit_data.mini_arena;
        REQUIRE(mini_arena[lit_data.mini_root].type == NodeType::MiniPattern);
        // MiniPattern should have 2 sample atoms: "bd" and "sd"
        CHECK(mini_arena.child_count(lit_data.mini_root) == 2);
    }

    // PRD prd-remove-pat-builtin: the `pat("…", closure)` form is gone.
    // The per-event callback role is covered by `poly()` consuming a typed
    // literal — exercised in the polyphony tests.
}

TEST_CASE("Parser complex expressions", "[parser]") {
    SECTION("math with multiple operators") {
        auto ast = parse_ok("400 + 300 * co");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ident_text(ast.arena[expr].as_identifier()) == "add");
    }

    SECTION("parenthesized expression") {
        auto ast = parse_ok("(1 + 2) * 3");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ident_text(ast.arena[expr].as_identifier()) == "mul");

        // First arg should be add
        NodeIndex first_arg = ast.arena[expr].first_child;
        NodeIndex add = ast.arena[first_arg].first_child;
        REQUIRE(ast.arena[add].type == NodeType::Call);
        CHECK(ident_text(ast.arena[add].as_identifier()) == "add");
    }

    SECTION("pipe with math") {
        auto ast = parse_ok("x |> % + % * 0.5");
        NodeIndex pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);
    }

    SECTION("realistic example") {
        auto ast = parse_ok("saw(440) |> lp(%, 1000) |> % * 0.5");
        NodeIndex outer_pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[outer_pipe].type == NodeType::Pipe);
    }
}

TEST_CASE("Parser multiple statements", "[parser]") {
    SECTION("multiple assignments") {
        auto ast = parse_ok("x = 1\ny = 2");
        NodeIndex root = ast.root;
        CHECK(ast.arena.child_count(root) == 2);
    }

    SECTION("assignment and expression") {
        auto ast = parse_ok("bpm = 120\nsaw(440)");
        NodeIndex root = ast.root;
        CHECK(ast.arena.child_count(root) == 2);

        NodeIndex first = ast.arena[root].first_child;
        NodeIndex second = ast.arena[first].next_sibling;
        CHECK(ast.arena[first].type == NodeType::Assignment);
        CHECK(ast.arena[second].type == NodeType::Call);
    }
}

TEST_CASE("Parser error handling", "[parser]") {
    SECTION("missing closing paren") {
        auto [tokens, lex_diags] = lex("foo(1, 2");
        auto [ast, parse_diags] = parse(std::move(tokens), "foo(1, 2");
        CHECK(!parse_diags.empty());
    }

    SECTION("missing expression") {
        auto [tokens, lex_diags] = lex("x = ");
        auto [ast, parse_diags] = parse(std::move(tokens), "x = ");
        CHECK(!parse_diags.empty());
    }

    SECTION("invalid token") {
        auto [tokens, lex_diags] = lex("x $ y");  // $ is not a valid token
        auto [ast, parse_diags] = parse(std::move(tokens), "x $ y");
        // Should either lex error or parse error
        bool has_error = !lex_diags.empty() || !parse_diags.empty();
        CHECK(has_error);
    }
}

TEST_CASE("Parser error recovery", "[parser]") {
    SECTION("recovers after missing closing bracket") {
        // Parser should recover and continue parsing after error
        auto [tokens, lex_diags] = lex("[1, 2 \n x = 3");
        auto [ast, parse_diags] = parse(std::move(tokens), "[1, 2 \n x = 3");
        // Should have error but possibly continue
        CHECK(!parse_diags.empty());
    }

    SECTION("recovers after malformed function call") {
        auto [tokens, lex_diags] = lex("foo(, )\nbar(1)");
        auto [ast, parse_diags] = parse(std::move(tokens), "foo(, )\nbar(1)");
        CHECK(!parse_diags.empty());
    }

    SECTION("error on multiple consecutive operators") {
        auto [tokens, lex_diags] = lex("1 + + 2");
        auto [ast, parse_diags] = parse(std::move(tokens), "1 + + 2");
        // Depending on implementation, might parse as 1 + (+2) or error
        // Either way, should not crash
    }

    SECTION("error on missing match braces") {
        auto [tokens, lex_diags] = lex("match(x) { \"a\": 1");
        auto [ast, parse_diags] = parse(std::move(tokens), "match(x) { \"a\": 1");
        CHECK(!parse_diags.empty());
    }

    SECTION("error on unclosed string") {
        auto [tokens, lex_diags] = lex("\"unclosed");
        // Lexer should produce error
        CHECK(!lex_diags.empty());
    }

    SECTION("error on invalid assignment target") {
        auto [tokens, lex_diags] = lex("42 = x");
        auto [ast, parse_diags] = parse(std::move(tokens), "42 = x");
        // Should produce error for invalid LHS
        bool has_error = !lex_diags.empty() || !parse_diags.empty();
        CHECK(has_error);
    }

    SECTION("error on missing arrow in closure") {
        auto [tokens, lex_diags] = lex("(x) 42");
        auto [ast, parse_diags] = parse(std::move(tokens), "(x) 42");
        // Missing -> should produce error or unexpected parse
        // Just verify no crash
    }

    SECTION("error on empty braces in non-record context") {
        // Empty match body
        auto [tokens, lex_diags] = lex("match(x) {}");
        auto [ast, parse_diags] = parse(std::move(tokens), "match(x) {}");
        // Should handle gracefully (either valid empty or error)
    }
}

TEST_CASE("Parser method calls", "[parser]") {
    SECTION("simple method call") {
        auto ast = parse_ok("x.foo()");
        NodeIndex method = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[method].as_identifier()) == "foo");

        // Should have receiver as first child (x)
        NodeIndex receiver = ast.arena[method].first_child;
        REQUIRE(ast.arena[receiver].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[receiver].as_identifier()) == "x");

        // No additional arguments
        CHECK(ast.arena[receiver].next_sibling == NULL_NODE);
    }

    SECTION("method call with arguments") {
        auto ast = parse_ok("osc.filter(1000, 0.5)");
        NodeIndex method = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[method].as_identifier()) == "filter");

        // First child is receiver, then arguments
        NodeIndex receiver = ast.arena[method].first_child;
        REQUIRE(ast.arena[receiver].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[receiver].as_identifier()) == "osc");

        // Two arguments after receiver
        NodeIndex arg1 = ast.arena[receiver].next_sibling;
        NodeIndex arg2 = ast.arena[arg1].next_sibling;
        REQUIRE(arg1 != NULL_NODE);
        REQUIRE(arg2 != NULL_NODE);
        CHECK(ast.arena[arg2].next_sibling == NULL_NODE);
    }

    SECTION("chained method calls") {
        auto ast = parse_ok("x.foo().bar()");
        NodeIndex outer = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[outer].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[outer].as_identifier()) == "bar");

        // Receiver should be inner method call
        NodeIndex inner = ast.arena[outer].first_child;
        REQUIRE(ast.arena[inner].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[inner].as_identifier()) == "foo");

        // Inner receiver should be x
        NodeIndex x = ast.arena[inner].first_child;
        REQUIRE(ast.arena[x].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[x].as_identifier()) == "x");
    }

    SECTION("method call on function result") {
        auto ast = parse_ok("foo(1).bar()");
        NodeIndex method = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[method].as_identifier()) == "bar");

        // Receiver should be function call
        NodeIndex call = ast.arena[method].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);
        CHECK(ident_text(ast.arena[call].as_identifier()) == "foo");
    }

    SECTION("method call with pipe") {
        auto ast = parse_ok("saw(440) |> %.filter(1000)");
        NodeIndex pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        // Second part should be method call
        NodeIndex lhs = ast.arena[pipe].first_child;
        NodeIndex rhs = ast.arena[lhs].next_sibling;
        REQUIRE(ast.arena[rhs].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[rhs].as_identifier()) == "filter");

        // Receiver should be hole
        NodeIndex receiver = ast.arena[rhs].first_child;
        CHECK(ast.arena[receiver].type == NodeType::Hole);
    }

    SECTION("method call mixed with operators") {
        auto ast = parse_ok("x.foo() + y.bar()");
        NodeIndex add = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[add].type == NodeType::Call);
        CHECK(ident_text(ast.arena[add].as_identifier()) == "add");

        // Both arguments should be method calls (wrapped in Argument nodes)
        NodeIndex arg1 = ast.arena[add].first_child;
        NodeIndex arg2 = ast.arena[arg1].next_sibling;

        NodeIndex method1 = ast.arena[arg1].first_child;
        NodeIndex method2 = ast.arena[arg2].first_child;

        REQUIRE(ast.arena[method1].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[method1].as_identifier()) == "foo");

        REQUIRE(ast.arena[method2].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[method2].as_identifier()) == "bar");
    }
}

TEST_CASE("Parser match expressions", "[parser]") {
    SECTION("simple match with string patterns") {
        auto ast = parse_ok("match(\"sin\") { \"sin\": 1, \"saw\": 2, _: 0 }");
        NodeIndex match = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[match].type == NodeType::MatchExpr);

        // First child is scrutinee
        NodeIndex scrutinee = ast.arena[match].first_child;
        REQUIRE(ast.arena[scrutinee].type == NodeType::StringLit);
        CHECK(ast.arena[scrutinee].as_string() == "sin");

        // Should have 3 arms
        std::size_t arm_count = 0;
        NodeIndex arm = ast.arena[scrutinee].next_sibling;
        while (arm != NULL_NODE) {
            REQUIRE(ast.arena[arm].type == NodeType::MatchArm);
            arm_count++;
            arm = ast.arena[arm].next_sibling;
        }
        CHECK(arm_count == 3);
    }

    SECTION("match with number patterns") {
        auto ast = parse_ok(R"(
            match(1) {
                1: "one"
                2: "two"
                _: "other"
            }
        )");
        NodeIndex match = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[match].type == NodeType::MatchExpr);

        NodeIndex scrutinee = ast.arena[match].first_child;
        REQUIRE(ast.arena[scrutinee].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[scrutinee].as_number(), WithinRel(1.0));
    }

    SECTION("match with block body") {
        auto ast = parse_ok(R"(
            match("x") {
                "x": { y = 1
                       y + 2 }
                _: 0
            }
        )");
        NodeIndex match = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[match].type == NodeType::MatchExpr);

        // First arm's body should be a block
        NodeIndex scrutinee = ast.arena[match].first_child;
        NodeIndex arm = ast.arena[scrutinee].next_sibling;
        NodeIndex pattern = ast.arena[arm].first_child;
        NodeIndex body = ast.arena[pattern].next_sibling;
        CHECK(ast.arena[body].type == NodeType::Block);
    }

    SECTION("match with wildcard") {
        auto ast = parse_ok(R"(
            match("unknown") {
                _: 42
            }
        )");
        NodeIndex match = ast.arena[ast.root].first_child;
        NodeIndex scrutinee = ast.arena[match].first_child;
        NodeIndex arm = ast.arena[scrutinee].next_sibling;

        REQUIRE(ast.arena[arm].type == NodeType::MatchArm);
        CHECK(ast.arena[arm].as_match_arm().is_wildcard == true);
    }

    SECTION("match non-wildcard pattern") {
        auto ast = parse_ok(R"(
            match("test") {
                "test": 1
            }
        )");
        NodeIndex match = ast.arena[ast.root].first_child;
        NodeIndex scrutinee = ast.arena[match].first_child;
        NodeIndex arm = ast.arena[scrutinee].next_sibling;

        REQUIRE(ast.arena[arm].type == NodeType::MatchArm);
        CHECK(ast.arena[arm].as_match_arm().is_wildcard == false);
    }
}

TEST_CASE("Parser match destructuring", "[parser][destructure]") {
    SECTION("destructuring pattern with two fields") {
        auto ast = parse_ok(R"(
            match(r) {
                {freq, vel}: freq * vel
                _: 0
            }
        )");
        NodeIndex match = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[match].type == NodeType::MatchExpr);

        NodeIndex scrutinee = ast.arena[match].first_child;
        NodeIndex arm = ast.arena[scrutinee].next_sibling;
        REQUIRE(ast.arena[arm].type == NodeType::MatchArm);

        const auto& arm_data = ast.arena[arm].as_match_arm();
        CHECK(arm_data.is_destructure == true);
        CHECK(arm_data.is_wildcard == false);
        REQUIRE(arm_data.destructure_fields.size() == 2);
        CHECK(arm_data.destructure_fields[0] == "freq");
        CHECK(arm_data.destructure_fields[1] == "vel");
    }

    SECTION("destructuring pattern with guard") {
        auto ast = parse_ok(R"(
            match(r) {
                {freq, vel} && vel > 0.5: freq
                _: 0
            }
        )");
        NodeIndex match = ast.arena[ast.root].first_child;
        NodeIndex scrutinee = ast.arena[match].first_child;
        NodeIndex arm = ast.arena[scrutinee].next_sibling;

        const auto& arm_data = ast.arena[arm].as_match_arm();
        CHECK(arm_data.is_destructure == true);
        CHECK(arm_data.has_guard == true);
        CHECK(arm_data.guard_node != NULL_NODE);
        REQUIRE(arm_data.destructure_fields.size() == 2);
        CHECK(arm_data.destructure_fields[0] == "freq");
        CHECK(arm_data.destructure_fields[1] == "vel");
    }

    SECTION("single field destructuring") {
        auto ast = parse_ok(R"(
            match(r) {
                {freq}: freq
                _: 0
            }
        )");
        NodeIndex match = ast.arena[ast.root].first_child;
        NodeIndex scrutinee = ast.arena[match].first_child;
        NodeIndex arm = ast.arena[scrutinee].next_sibling;

        const auto& arm_data = ast.arena[arm].as_match_arm();
        CHECK(arm_data.is_destructure == true);
        REQUIRE(arm_data.destructure_fields.size() == 1);
        CHECK(arm_data.destructure_fields[0] == "freq");
    }
}

TEST_CASE("Parser as destructuring", "[parser][destructure]") {
    SECTION("as destructuring binding") {
        auto ast = parse_ok("1 as {freq, vel} |> freq + vel");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        NodeIndex lhs = ast.arena[pipe].first_child;
        REQUIRE(ast.arena[lhs].type == NodeType::PipeBinding);

        const auto& binding = ast.arena[lhs].as_pipe_binding();
        // Should have auto-generated temp name
        CHECK(binding.binding_name.substr(0, 8) == "__destr_");
        REQUIRE(binding.destructure_fields.size() == 2);
        CHECK(binding.destructure_fields[0] == "freq");
        CHECK(binding.destructure_fields[1] == "vel");
    }

    SECTION("as destructuring single field") {
        auto ast = parse_ok("1 as {x} |> x");
        NodeIndex pipe = ast.arena[ast.root].first_child;
        NodeIndex lhs = ast.arena[pipe].first_child;
        REQUIRE(ast.arena[lhs].type == NodeType::PipeBinding);

        const auto& binding = ast.arena[lhs].as_pipe_binding();
        REQUIRE(binding.destructure_fields.size() == 1);
        CHECK(binding.destructure_fields[0] == "x");
    }
}

TEST_CASE("Parser statement-level destructure assignment", "[parser][destructure]") {
    SECTION("two-field destructure assignment") {
        auto ast = parse_ok(R"(
            r = {a: 1, b: 2}
            {a, b} = r
        )");
        // Find the DestructureAssignment node (last top-level statement)
        NodeIndex stmt = ast.arena[ast.root].first_child;
        while (ast.arena[stmt].next_sibling != NULL_NODE) {
            stmt = ast.arena[stmt].next_sibling;
        }
        REQUIRE(ast.arena[stmt].type == NodeType::DestructureAssignment);

        const auto& dd = ast.arena[stmt].as_destructure_assignment();
        REQUIRE(dd.fields.size() == 2);
        CHECK(dd.fields[0].name == "a");
        CHECK(dd.fields[1].name == "b");
        CHECK(dd.fields[0].default_node == NULL_NODE);
        CHECK(dd.fields[1].default_node == NULL_NODE);

        // RHS should be the first child (an Identifier referencing `r`)
        NodeIndex rhs = ast.arena[stmt].first_child;
        REQUIRE(rhs != NULL_NODE);
        REQUIRE(ast.arena[rhs].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[rhs].as_identifier()) == "r");
    }

    SECTION("single-field destructure assignment") {
        auto ast = parse_ok(R"(
            r = {x: 5}
            {x} = r
        )");
        NodeIndex stmt = ast.arena[ast.root].first_child;
        while (ast.arena[stmt].next_sibling != NULL_NODE) {
            stmt = ast.arena[stmt].next_sibling;
        }
        REQUIRE(ast.arena[stmt].type == NodeType::DestructureAssignment);
        const auto& dd = ast.arena[stmt].as_destructure_assignment();
        REQUIRE(dd.fields.size() == 1);
        CHECK(dd.fields[0].name == "x");
        CHECK(dd.fields[0].default_node == NULL_NODE);
    }

    SECTION("destructure RHS can be a complex expression") {
        auto ast = parse_ok(R"(
            {x, y} = {x: 1, y: 2}
        )");
        NodeIndex stmt = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[stmt].type == NodeType::DestructureAssignment);
        NodeIndex rhs = ast.arena[stmt].first_child;
        REQUIRE(ast.arena[rhs].type == NodeType::RecordLit);
    }

    SECTION("record literal as expression-statement is NOT a destructure") {
        // {a: 1} (with colon) should parse as a record-literal expression statement,
        // not a destructure assignment.
        auto ast = parse_ok("{a: 1}");
        NodeIndex stmt = ast.arena[ast.root].first_child;
        // Whatever it is, it must NOT be DestructureAssignment.
        CHECK(ast.arena[stmt].type != NodeType::DestructureAssignment);
    }

    SECTION("duplicate field name emits E188") {
        auto [tokens, lex_diags] = lex("{x, x} = r");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "{x, x} = r");

        bool got_e188 = false;
        for (const auto& d : parse_diags) {
            if (d.code == "E188") got_e188 = true;
        }
        CHECK(got_e188);
    }
}

TEST_CASE("Parser destructure defaults (statement-level)", "[parser][destructure]") {
    SECTION("single-field default") {
        auto ast = parse_ok(R"(
            r = {a: 1}
            {a = 99} = r
        )");
        // Last top-level statement is the destructure assignment.
        NodeIndex stmt = ast.arena[ast.root].first_child;
        while (ast.arena[stmt].next_sibling != NULL_NODE) {
            stmt = ast.arena[stmt].next_sibling;
        }
        REQUIRE(ast.arena[stmt].type == NodeType::DestructureAssignment);
        const auto& dd = ast.arena[stmt].as_destructure_assignment();
        REQUIRE(dd.fields.size() == 1);
        CHECK(dd.fields[0].name == "a");
        CHECK(dd.fields[0].default_node != NULL_NODE);
        CHECK(ast.arena[dd.fields[0].default_node].type == NodeType::NumberLit);
    }

    SECTION("mixed defaults and required fields") {
        auto ast = parse_ok(R"(
            r = {a: 1, b: 2}
            {a = 0, b, c = "hi"} = r
        )");
        NodeIndex stmt = ast.arena[ast.root].first_child;
        while (ast.arena[stmt].next_sibling != NULL_NODE) {
            stmt = ast.arena[stmt].next_sibling;
        }
        REQUIRE(ast.arena[stmt].type == NodeType::DestructureAssignment);
        const auto& dd = ast.arena[stmt].as_destructure_assignment();
        REQUIRE(dd.fields.size() == 3);
        CHECK(dd.fields[0].name == "a");
        CHECK(dd.fields[0].default_node != NULL_NODE);
        CHECK(dd.fields[1].name == "b");
        CHECK(dd.fields[1].default_node == NULL_NODE);
        CHECK(dd.fields[2].name == "c");
        CHECK(dd.fields[2].default_node != NULL_NODE);
        CHECK(ast.arena[dd.fields[2].default_node].type == NodeType::StringLit);
    }

    SECTION("expression default with operator") {
        auto ast = parse_ok(R"(
            base = 100
            r = {a: 1}
            {a = base + 50} = r
        )");
        NodeIndex stmt = ast.arena[ast.root].first_child;
        while (ast.arena[stmt].next_sibling != NULL_NODE) {
            stmt = ast.arena[stmt].next_sibling;
        }
        REQUIRE(ast.arena[stmt].type == NodeType::DestructureAssignment);
        const auto& dd = ast.arena[stmt].as_destructure_assignment();
        REQUIRE(dd.fields.size() == 1);
        // Default is a Call node (binary `+` desugars to `add(...)`).
        REQUIRE(dd.fields[0].default_node != NULL_NODE);
        CHECK(ast.arena[dd.fields[0].default_node].type == NodeType::Call);
    }

    SECTION("disambiguator survives parens in default") {
        // `(a + b)` inside default — tracking depth is required so the
        // disambiguator finds the matching `}` followed by `=`.
        auto ast = parse_ok(R"(
            a = 2
            b = 3
            r = {x: 0}
            {x = (a + b)} = r
        )");
        NodeIndex stmt = ast.arena[ast.root].first_child;
        while (ast.arena[stmt].next_sibling != NULL_NODE) {
            stmt = ast.arena[stmt].next_sibling;
        }
        REQUIRE(ast.arena[stmt].type == NodeType::DestructureAssignment);
    }
}

TEST_CASE("Parser fn-param destructure", "[parser][destructure]") {
    SECTION("single destructure parameter") {
        auto ast = parse_ok(R"(
            fn f({x, y}) -> x + y
        )");
        // Top-level FunctionDef.
        NodeIndex fn_node = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn_node].type == NodeType::FunctionDef);
        CHECK(ast.arena[fn_node].as_function_def().param_count == 1);

        NodeIndex param_child = ast.arena[fn_node].first_child;
        REQUIRE(param_child != NULL_NODE);
        REQUIRE(ast.arena[param_child].type == NodeType::DestructureParam);
        const auto& dp = ast.arena[param_child].as_destructure_param();
        REQUIRE(dp.fields.size() == 2);
        CHECK(dp.fields[0].name == "x");
        CHECK(dp.fields[1].name == "y");
        CHECK(dp.fields[0].default_node == NULL_NODE);
        CHECK(dp.fields[1].default_node == NULL_NODE);
    }

    SECTION("destructure parameter with defaults") {
        auto ast = parse_ok(R"(
            fn synth({freq = 440, wave = "saw", q = 0.7}) -> osc(wave, freq)
        )");
        NodeIndex fn_node = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn_node].type == NodeType::FunctionDef);
        NodeIndex param_child = ast.arena[fn_node].first_child;
        REQUIRE(ast.arena[param_child].type == NodeType::DestructureParam);
        const auto& dp = ast.arena[param_child].as_destructure_param();
        REQUIRE(dp.fields.size() == 3);
        CHECK(dp.fields[0].name == "freq");
        CHECK(dp.fields[0].default_node != NULL_NODE);
        CHECK(dp.fields[1].name == "wave");
        CHECK(dp.fields[1].default_node != NULL_NODE);
        CHECK(dp.fields[2].name == "q");
        CHECK(dp.fields[2].default_node != NULL_NODE);
    }

    SECTION("destructure mixed with regular params") {
        auto ast = parse_ok(R"(
            fn lp_voice(freq, {cutoff, q = 0.7}) -> saw(freq)
        )");
        NodeIndex fn_node = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn_node].type == NodeType::FunctionDef);
        CHECK(ast.arena[fn_node].as_function_def().param_count == 2);

        NodeIndex first_param = ast.arena[fn_node].first_child;
        REQUIRE(first_param != NULL_NODE);
        // First param is a normal Identifier ("freq").
        CHECK(ast.arena[first_param].type == NodeType::Identifier);

        NodeIndex second_param = ast.arena[first_param].next_sibling;
        REQUIRE(second_param != NULL_NODE);
        CHECK(ast.arena[second_param].type == NodeType::DestructureParam);
        const auto& dp = ast.arena[second_param].as_destructure_param();
        REQUIRE(dp.fields.size() == 2);
        CHECK(dp.fields[0].name == "cutoff");
        CHECK(dp.fields[1].name == "q");
        CHECK(dp.fields[1].default_node != NULL_NODE);
    }
}

TEST_CASE("Parser rejects deferred destructure forms", "[parser][destructure]") {
    SECTION("defaults in pipe-binding `as {x = 1}` is a parse error") {
        auto [tokens, lex_diags] = lex(R"(
            n"c4" as {freq = 440} |> sine(freq)
        )");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "src");
        // We expect a parse diagnostic flagging the default in this context.
        bool got_error = false;
        for (const auto& d : parse_diags) {
            if (d.severity == Severity::Error &&
                d.message.find("Default values are not allowed") != std::string::npos) {
                got_error = true;
            }
        }
        CHECK(got_error);
    }

    SECTION("defaults in match-arm destructure are a parse error") {
        auto [tokens, lex_diags] = lex(R"(
            r = {x: 1}
            match (r) {
                {x = 0}: x,
                _: 0
            }
        )");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "src");
        bool got_error = false;
        for (const auto& d : parse_diags) {
            if (d.severity == Severity::Error &&
                d.message.find("Default values are not allowed") != std::string::npos) {
                got_error = true;
            }
        }
        CHECK(got_error);
    }

    SECTION("destructure parameter in closure parses cleanly") {
        // prd-poly-callback-event-record: closures accept `{x, y}` destructure
        // params, same as `fn` definitions. The closure-detection heuristic
        // recognizes `{` as a param start.
        auto [tokens, lex_diags] = lex(R"(
            f = ({x, y}) -> x + y
        )");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "src");
        bool any_error = false;
        for (const auto& d : parse_diags) {
            if (d.severity == Severity::Error) any_error = true;
        }
        CHECK_FALSE(any_error);

        // The closure carries a DestructureParam node before its body.
        bool found_destructure = false;
        for (std::size_t i = 0; i < ast.arena.size(); ++i) {
            if (ast.arena[static_cast<NodeIndex>(i)].type ==
                NodeType::DestructureParam) {
                found_destructure = true;
            }
        }
        CHECK(found_destructure);
    }
}

TEST_CASE("Parser arrays", "[parser][array]") {
    SECTION("empty array") {
        auto ast = parse_ok("[]");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::ArrayLit);
        CHECK(ast.arena.child_count(child) == 0);
    }

    SECTION("single element array") {
        auto ast = parse_ok("[42]");
        NodeIndex arr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);
        CHECK(ast.arena.child_count(arr) == 1);

        NodeIndex elem = ast.arena[arr].first_child;
        REQUIRE(ast.arena[elem].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[elem].as_number(), WithinRel(42.0));
    }

    SECTION("multiple element array") {
        auto ast = parse_ok("[1, 2, 3]");
        NodeIndex arr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);
        CHECK(ast.arena.child_count(arr) == 3);
    }

    SECTION("array with mixed types") {
        auto ast = parse_ok("[1, \"hello\", true]");
        NodeIndex arr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);
        CHECK(ast.arena.child_count(arr) == 3);

        NodeIndex elem1 = ast.arena[arr].first_child;
        NodeIndex elem2 = ast.arena[elem1].next_sibling;
        NodeIndex elem3 = ast.arena[elem2].next_sibling;

        CHECK(ast.arena[elem1].type == NodeType::NumberLit);
        CHECK(ast.arena[elem2].type == NodeType::StringLit);
        CHECK(ast.arena[elem3].type == NodeType::BoolLit);
    }

    SECTION("array with expressions") {
        auto ast = parse_ok("[1 + 2, foo(x)]");
        NodeIndex arr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);
        CHECK(ast.arena.child_count(arr) == 2);

        NodeIndex elem1 = ast.arena[arr].first_child;
        NodeIndex elem2 = ast.arena[elem1].next_sibling;

        REQUIRE(ast.arena[elem1].type == NodeType::Call);
        CHECK(ident_text(ast.arena[elem1].as_identifier()) == "add");

        REQUIRE(ast.arena[elem2].type == NodeType::Call);
        CHECK(ident_text(ast.arena[elem2].as_identifier()) == "foo");
    }

    SECTION("nested arrays") {
        auto ast = parse_ok("[[1, 2], [3, 4]]");
        NodeIndex outer = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[outer].type == NodeType::ArrayLit);
        CHECK(ast.arena.child_count(outer) == 2);

        NodeIndex inner1 = ast.arena[outer].first_child;
        NodeIndex inner2 = ast.arena[inner1].next_sibling;

        REQUIRE(ast.arena[inner1].type == NodeType::ArrayLit);
        REQUIRE(ast.arena[inner2].type == NodeType::ArrayLit);
        CHECK(ast.arena.child_count(inner1) == 2);
        CHECK(ast.arena.child_count(inner2) == 2);
    }

    SECTION("array assignment") {
        // `arr` is now reserved (Phase 2). Use `xs` as variable name instead.
        auto ast = parse_ok("xs = [1, 2, 3]");
        NodeIndex assign = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[assign].type == NodeType::Assignment);
        CHECK(ident_text(ast.arena[assign].as_identifier()) == "xs");

        NodeIndex value = ast.arena[assign].first_child;
        REQUIRE(ast.arena[value].type == NodeType::ArrayLit);
    }

    SECTION("array as function argument") {
        auto ast = parse_ok("foo([1, 2, 3])");
        NodeIndex call = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        NodeIndex arg = ast.arena[call].first_child;
        REQUIRE(ast.arena[arg].type == NodeType::Argument);

        NodeIndex arr = ast.arena[arg].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);
    }

    SECTION("array in pipe") {
        auto ast = parse_ok("[1, 2, 3] |> foo(%)");
        NodeIndex pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        NodeIndex arr = ast.arena[pipe].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);
    }

    SECTION("array indexing with number") {
        // `arr` is now reserved (Phase 2). Use `xs` as variable name instead.
        auto ast = parse_ok("xs[0]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);
        CHECK(ast.arena.child_count(index) == 2);

        NodeIndex arr = ast.arena[index].first_child;
        NodeIndex idx_expr = ast.arena[arr].next_sibling;

        REQUIRE(ast.arena[arr].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[arr].as_identifier()) == "xs");

        REQUIRE(ast.arena[idx_expr].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[idx_expr].as_number(), WithinRel(0.0));
    }

    SECTION("array indexing with variable") {
        auto ast = parse_ok("xs[i]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);

        NodeIndex arr = ast.arena[index].first_child;
        NodeIndex idx_expr = ast.arena[arr].next_sibling;

        REQUIRE(ast.arena[idx_expr].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[idx_expr].as_identifier()) == "i");
    }

    SECTION("array indexing with expression") {
        auto ast = parse_ok("xs[i + 1]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);

        NodeIndex arr = ast.arena[index].first_child;
        NodeIndex idx_expr = ast.arena[arr].next_sibling;

        REQUIRE(ast.arena[idx_expr].type == NodeType::Call);
        CHECK(ident_text(ast.arena[idx_expr].as_identifier()) == "add");
    }

    SECTION("chained indexing") {
        auto ast = parse_ok("xs[0][1]");
        NodeIndex outer = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[outer].type == NodeType::Index);

        NodeIndex inner = ast.arena[outer].first_child;
        REQUIRE(ast.arena[inner].type == NodeType::Index);
    }

    SECTION("indexing on array literal") {
        auto ast = parse_ok("[1, 2, 3][0]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);

        NodeIndex arr = ast.arena[index].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);
    }

    SECTION("indexing on function call") {
        auto ast = parse_ok("foo()[0]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);

        NodeIndex call = ast.arena[index].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);
        CHECK(ident_text(ast.arena[call].as_identifier()) == "foo");
    }

    SECTION("method call on indexed value") {
        auto ast = parse_ok("xs[0].foo()");
        NodeIndex method = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
        CHECK(ident_text(ast.arena[method].as_identifier()) == "foo");

        NodeIndex index = ast.arena[method].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);
    }

    SECTION("indexing after method call") {
        auto ast = parse_ok("foo.bar()[0]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);

        NodeIndex method = ast.arena[index].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
    }
}

TEST_CASE("Parser function definitions", "[parser]") {
    SECTION("simple function") {
        auto ast = parse_ok("fn double(x) -> x * 2");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        const auto& fn_data = ast.arena[fn].as_function_def();
        CHECK(fn_data.name == "double");
        CHECK(fn_data.param_count == 1);

        // Check param
        NodeIndex param = ast.arena[fn].first_child;
        REQUIRE(ast.arena[param].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[param].as_identifier()) == "x");

        // Check body
        NodeIndex body = ast.arena[param].next_sibling;
        REQUIRE(ast.arena[body].type == NodeType::Call);
        CHECK(ident_text(ast.arena[body].as_identifier()) == "mul");
    }

    SECTION("function with multiple parameters") {
        auto ast = parse_ok("fn add3(a, b, c) -> a + b + c");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        const auto& fn_data = ast.arena[fn].as_function_def();
        CHECK(fn_data.name == "add3");
        CHECK(fn_data.param_count == 3);
    }

    SECTION("function with default parameter") {
        auto ast = parse_ok("fn osc(type, freq, pwm = 0.5) -> freq");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        const auto& fn_data = ast.arena[fn].as_function_def();
        CHECK(fn_data.name == "osc");
        CHECK(fn_data.param_count == 3);

        // Check third param has default
        NodeIndex param1 = ast.arena[fn].first_child;
        NodeIndex param2 = ast.arena[param1].next_sibling;
        NodeIndex param3 = ast.arena[param2].next_sibling;

        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param3].data));
        const auto& param3_data = ast.arena[param3].as_closure_param();
        CHECK(param3_data.name == "pwm");
        REQUIRE(param3_data.default_value.has_value());
        CHECK_THAT(*param3_data.default_value, WithinRel(0.5));
    }

    SECTION("function with block body") {
        auto ast = parse_ok(R"(
            fn complex(x) -> {
                y = x * 2
                y + 1
            }
        )");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        NodeIndex param = ast.arena[fn].first_child;
        NodeIndex body = ast.arena[param].next_sibling;
        CHECK(ast.arena[body].type == NodeType::Block);
    }

    SECTION("function with match in body") {
        auto ast = parse_ok(R"(
            fn select(type) -> match(type) {
                "a": 1
                "b": 2
                _: 0
            }
        )");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        NodeIndex param = ast.arena[fn].first_child;
        NodeIndex body = ast.arena[param].next_sibling;
        CHECK(ast.arena[body].type == NodeType::MatchExpr);
    }

    SECTION("multiple function definitions") {
        auto ast = parse_ok(R"(
            fn foo(x) -> x
            fn bar(y) -> y * 2
        )");
        NodeIndex root = ast.root;
        CHECK(ast.arena.child_count(root) == 2);

        NodeIndex fn1 = ast.arena[root].first_child;
        NodeIndex fn2 = ast.arena[fn1].next_sibling;

        REQUIRE(ast.arena[fn1].type == NodeType::FunctionDef);
        REQUIRE(ast.arena[fn2].type == NodeType::FunctionDef);

        CHECK(ast.arena[fn1].as_function_def().name == "foo");
        CHECK(ast.arena[fn2].as_function_def().name == "bar");
    }
}

// =============================================================================
// PRD prd-parameter-type-annotations §3.1: `name: type` annotation grammar on
// fn parameter lists. Type names are uppercase PascalCase identifiers mirroring
// the C++ ValueType enum: `Signal`, `Number`, `Pattern`, `Record`, `Array`,
// `String`, `Function`, `Stream`. They are NOT keywords — they lex as plain
// identifiers and are resolved contextually only in annotation position. Tests
// in this section cover the parser/AST surface (grammar, AST-node promotion,
// E104/E185 diagnostics). Codegen-side behavior (E184 + symbol-binding) is
// covered in test_param_type_annotations.cpp.
// =============================================================================

TEST_CASE("Parser fn parameter type annotations: grammar",
          "[parser][type-annotation]") {
    SECTION("simple : Stream annotation parses and promotes to ClosureParamData") {
        auto ast = parse_ok("fn transpose(events: Stream, n) -> events");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(ast.arena[p0].type == NodeType::Identifier);
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        const auto& cp0 = ast.arena[p0].as_closure_param();
        CHECK(cp0.name == "events");
        CHECK(cp0.annotated_type == ParamValueType::Stream);
        CHECK_FALSE(cp0.default_value.has_value());

        NodeIndex p1 = ast.arena[p0].next_sibling;
        REQUIRE(ast.arena[p1].type == NodeType::Identifier);
        // Second param is unannotated and stays IdentifierData (cheap path).
        REQUIRE(std::holds_alternative<Node::IdentifierData>(ast.arena[p1].data));
        CHECK(ident_text(ast.arena[p1].as_identifier()) == "n");
    }

    SECTION(": Signal annotation parses to ParamValueType::Signal") {
        auto ast = parse_ok("fn wobble(rate: Signal, depth) -> depth");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        CHECK(ast.arena[p0].as_closure_param().annotated_type == ParamValueType::Signal);
    }

    SECTION(": Pattern annotation parses to ParamValueType::Pattern") {
        auto ast = parse_ok("fn play(p: Pattern, gain) -> gain");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        CHECK(ast.arena[p0].as_closure_param().annotated_type == ParamValueType::Pattern);
    }

    SECTION(": Number annotation parses to ParamValueType::Number") {
        auto ast = parse_ok("fn unison(freq, voices: Number) -> freq");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p1 = ast.arena[ast.arena[fn].first_child].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p1].data));
        CHECK(ast.arena[p1].as_closure_param().annotated_type == ParamValueType::Number);
    }

    SECTION(": Record annotation parses to ParamValueType::Record") {
        auto ast = parse_ok("fn arpinst(e: Record) -> e");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        CHECK(ast.arena[p0].as_closure_param().annotated_type == ParamValueType::Record);
    }

    SECTION(": Array annotation parses to ParamValueType::Array") {
        auto ast = parse_ok("fn mixer(channels: Array, gain) -> gain");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        CHECK(ast.arena[p0].as_closure_param().annotated_type == ParamValueType::Array);
    }

    SECTION(": String annotation parses to ParamValueType::String") {
        auto ast = parse_ok("fn osctype(kind: String, freq) -> freq");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        CHECK(ast.arena[p0].as_closure_param().annotated_type == ParamValueType::String);
    }

    SECTION(": Function annotation parses to ParamValueType::Function") {
        auto ast = parse_ok("fn each_voice(voices, cb: Function) -> voices");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p1 = ast.arena[ast.arena[fn].first_child].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p1].data));
        CHECK(ast.arena[p1].as_closure_param().annotated_type == ParamValueType::Function);
    }

    SECTION("annotation precedes default value") {
        auto ast = parse_ok("fn f(rate: Signal = 220) -> rate");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        const auto& cp = ast.arena[p0].as_closure_param();
        CHECK(cp.annotated_type == ParamValueType::Signal);
        REQUIRE(cp.default_value.has_value());
        CHECK_THAT(*cp.default_value, WithinRel(220.0));
    }

    SECTION(": Number + numeric default compose") {
        auto ast = parse_ok("fn f(voices: Number = 4) -> voices");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        const auto& cp = ast.arena[p0].as_closure_param();
        CHECK(cp.annotated_type == ParamValueType::Number);
        REQUIRE(cp.default_value.has_value());
        CHECK_THAT(*cp.default_value, WithinRel(4.0));
    }

    SECTION("annotation accepted before numeric default (eval errors deferred)") {
        // The parser accepts the grammar `name : type = default`. A `: Stream`
        // default of `0` is semantically meaningless (PRD §8.9) but the
        // parser doesn't reject it — that's deferred to body-side usage
        // diagnostics. Verifies the token order `: type =`.
        auto ast = parse_ok("fn t(events: Stream = 0) -> events");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[p0].data));
        const auto& cp = ast.arena[p0].as_closure_param();
        CHECK(cp.annotated_type == ParamValueType::Stream);
        REQUIRE(cp.default_value.has_value());
    }

    SECTION("multiple annotated params") {
        auto ast = parse_ok(
            "fn f(e: Stream, r: Signal, n: Number, k: String, cb: Function) -> n");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        NodeIndex p1 = ast.arena[p0].next_sibling;
        NodeIndex p2 = ast.arena[p1].next_sibling;
        NodeIndex p3 = ast.arena[p2].next_sibling;
        NodeIndex p4 = ast.arena[p3].next_sibling;
        CHECK(ast.arena[p0].as_closure_param().annotated_type == ParamValueType::Stream);
        CHECK(ast.arena[p1].as_closure_param().annotated_type == ParamValueType::Signal);
        CHECK(ast.arena[p2].as_closure_param().annotated_type == ParamValueType::Number);
        CHECK(ast.arena[p3].as_closure_param().annotated_type == ParamValueType::String);
        CHECK(ast.arena[p4].as_closure_param().annotated_type == ParamValueType::Function);
    }

    SECTION("unannotated params keep IdentifierData (no promotion)") {
        auto ast = parse_ok("fn f(x, y) -> x + y");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        NodeIndex p1 = ast.arena[p0].next_sibling;
        // Regression: zero overhead for un-annotated params.
        CHECK(std::holds_alternative<Node::IdentifierData>(ast.arena[p0].data));
        CHECK(std::holds_alternative<Node::IdentifierData>(ast.arena[p1].data));
    }
}

TEST_CASE("Parser fn parameter type annotations: E185 unknown type name",
          "[parser][type-annotation]") {
    SECTION("E185 fires on unknown annotation keyword") {
        auto [tokens, lex_diags] = lex("fn f(events: bogustype) -> events");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "src");
        REQUIRE_FALSE(parse_diags.empty());
        bool has_e185 = false;
        for (const auto& d : parse_diags) {
            if (d.code == "E185") {
                has_e185 = true;
                // Diagnostic should enumerate the valid type names as a hint.
                CHECK(d.message.find("Signal") != std::string::npos);
            }
        }
        CHECK(has_e185);
    }

    SECTION("E185 fires on lowercase type names (only PascalCase is valid)") {
        // Type names are case-sensitive: `stream`/`signal`/`num` are NOT
        // annotation type names — they lex as plain Identifiers and fall
        // through to the E185 path in the annotation lambda. Only the
        // uppercase forms (`Stream`, `Signal`, `Number`, …) are accepted.
        for (const char* src : {"fn f(events: stream) -> events",
                                 "fn f(rate: signal) -> rate",
                                 "fn f(n: num) -> n"}) {
            auto [tokens, lex_diags] = lex(src);
            REQUIRE(lex_diags.empty());
            auto [ast, parse_diags] = parse(std::move(tokens), "src");
            REQUIRE_FALSE(parse_diags.empty());
            bool has_e185 = false;
            for (const auto& d : parse_diags) {
                if (d.code == "E185") has_e185 = true;
            }
            CHECK(has_e185);
        }
    }
}

TEST_CASE("Parser fn parameter type annotations: E104 disallowed positions",
          "[parser][type-annotation]") {
    SECTION("E104 fires on destructure param annotation") {
        auto [tokens, lex_diags] = lex("fn f({x, y}: Record) -> x + y");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "src");
        REQUIRE_FALSE(parse_diags.empty());
        bool has_e104 = false;
        for (const auto& d : parse_diags) {
            if (d.code == "E104") has_e104 = true;
        }
        CHECK(has_e104);
    }

    SECTION("E104 fires on rest param annotation") {
        auto [tokens, lex_diags] = lex("fn f(...args: Array) -> args");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "src");
        REQUIRE_FALSE(parse_diags.empty());
        bool has_e104 = false;
        for (const auto& d : parse_diags) {
            if (d.code == "E104") has_e104 = true;
        }
        CHECK(has_e104);
    }
}

TEST_CASE("Parser: type names are not reserved identifiers",
          "[parser][type-annotation]") {
    // PRD prd-parameter-type-annotations: type names (and the former
    // abbreviations) are NOT keywords. They are ordinary identifiers and
    // remain fully usable as variable / fn / parameter names. Only in
    // annotation position (after a `:`) are the uppercase forms special.

    SECTION("former abbreviations are usable as identifiers") {
        // `arr`, `str`, `num`, `stream`, `sig` are plain identifiers now.
        auto ast = parse_ok("fn arr(x) -> x");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);
        CHECK(ast.arena[fn].as_function_def().name == "arr");
    }

    SECTION("former abbreviation usable as a parameter name") {
        auto ast = parse_ok("fn f(arr) -> arr");
        NodeIndex fn = ast.arena[ast.root].first_child;
        NodeIndex p0 = ast.arena[fn].first_child;
        CHECK(std::holds_alternative<Node::IdentifierData>(ast.arena[p0].data));
    }

    SECTION("uppercase type names are usable as identifiers outside annotation position") {
        // `String`/`Array`/etc. are not reserved — only meaningful after `:`.
        auto ast = parse_ok("fn String(x) -> x");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);
        CHECK(ast.arena[fn].as_function_def().name == "String");
    }
}

TEST_CASE("Parser fn arrow body does not swallow the next line",
          "[parser][fn]") {
    // The grammar is newline-insensitive and has no statement terminator.
    // A `(` only extends an identifier into a call when it is on the SAME
    // source line. Without that guard, a `fn ... -> body` whose body ends in
    // a bare identifier absorbs a following `(...)` statement as call args,
    // collapsing the whole program into the function body (and tripping a
    // false E240 recursion downstream).
    SECTION("bare-identifier body, next line opens with `(`") {
        auto ast = parse_ok(R"(
            fn vox(f, g) -> f * g
            (vox(1, 2) + vox(3, 4)) * 0.5 |> out(@)
        )");
        NodeIndex root = ast.root;
        CHECK(ast.arena.child_count(root) == 2);

        NodeIndex fn = ast.arena[root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);
        CHECK(ast.arena[fn].as_function_def().name == "vox");

        // The fn body is the FunctionDef's last child: `f * g` — the parser
        // desugars `*` to a `mul` Call. The bug made it a Pipe reaching into
        // the program's output statement; assert it stays a self-contained
        // `mul`.
        NodeIndex body = ast.arena[fn].first_child;
        while (ast.arena[body].next_sibling != NULL_NODE) {
            body = ast.arena[body].next_sibling;
        }
        REQUIRE(ast.arena[body].type == NodeType::Call);
        CHECK(ident_text(ast.arena[body].as_identifier()) == "mul");

        // The second top-level statement is the independent pipe.
        NodeIndex stmt2 = ast.arena[fn].next_sibling;
        CHECK(ast.arena[stmt2].type == NodeType::Pipe);
    }

    SECTION("a `(` on the same line as the name is still a call") {
        auto ast = parse_ok("fn f(x) -> g(x)\nf(1) |> out(@)");
        NodeIndex root = ast.root;
        CHECK(ast.arena.child_count(root) == 2);
        NodeIndex fn = ast.arena[root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);
        // Body `g(x)` is a Call — the same-line `(` still binds.
        NodeIndex body = ast.arena[fn].first_child;
        while (ast.arena[body].next_sibling != NULL_NODE) {
            body = ast.arena[body].next_sibling;
        }
        CHECK(ast.arena[body].type == NodeType::Call);
    }
}

TEST_CASE("Parser string default parameters", "[parser][fn]") {
    SECTION("function with string default") {
        auto ast = parse_ok(R"(fn osc(type = "sin", freq = 440) -> freq)");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        const auto& fn_data = ast.arena[fn].as_function_def();
        CHECK(fn_data.param_count == 2);

        // First param: string default
        NodeIndex param1 = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param1].data));
        const auto& p1 = ast.arena[param1].as_closure_param();
        CHECK(p1.name == "type");
        REQUIRE(p1.default_string.has_value());
        CHECK(*p1.default_string == "sin");
        CHECK_FALSE(p1.default_value.has_value());

        // Second param: numeric default
        NodeIndex param2 = ast.arena[param1].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param2].data));
        const auto& p2 = ast.arena[param2].as_closure_param();
        CHECK(p2.name == "freq");
        REQUIRE(p2.default_value.has_value());
        CHECK_THAT(*p2.default_value, WithinRel(440.0));
        CHECK_FALSE(p2.default_string.has_value());
    }
}

TEST_CASE("Parser rest parameters", "[parser][fn]") {
    SECTION("function with rest parameter") {
        auto ast = parse_ok("fn mix(...sigs) -> sigs");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        const auto& fn_data = ast.arena[fn].as_function_def();
        CHECK(fn_data.has_rest_param);
        CHECK(fn_data.param_count == 1);

        NodeIndex param = ast.arena[fn].first_child;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param].data));
        const auto& p = ast.arena[param].as_closure_param();
        CHECK(p.name == "sigs");
        CHECK(p.is_rest);
    }

    SECTION("rest param with required params before") {
        auto ast = parse_ok("fn mix(gain, ...sigs) -> sigs");
        NodeIndex fn = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[fn].type == NodeType::FunctionDef);

        const auto& fn_data = ast.arena[fn].as_function_def();
        CHECK(fn_data.has_rest_param);
        CHECK(fn_data.param_count == 2);

        // First param: regular
        NodeIndex param1 = ast.arena[fn].first_child;
        // Second param: rest
        NodeIndex param2 = ast.arena[param1].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param2].data));
        CHECK(ast.arena[param2].as_closure_param().is_rest);
    }
}

TEST_CASE("Parser underscore placeholder", "[parser]") {
    SECTION("underscore in call arguments") {
        auto ast = parse_ok("f(1, _, 3)");
        NodeIndex call = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        // Second argument should be Argument wrapping Identifier("_")
        NodeIndex arg1 = ast.arena[call].first_child;
        NodeIndex arg2 = ast.arena[arg1].next_sibling;
        REQUIRE(ast.arena[arg2].type == NodeType::Argument);
        NodeIndex inner = ast.arena[arg2].first_child;
        REQUIRE(ast.arena[inner].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[inner].as_identifier()) == "_");
    }

    SECTION("cannot assign to underscore") {
        auto [tokens, lex_diags] = lex("_ = 42");
        auto [ast, parse_diags] = parse(std::move(tokens), "_ = 42");
        CHECK(!parse_diags.empty());
    }

    SECTION("cannot const assign to underscore") {
        auto [tokens, lex_diags] = lex("const _ = 42");
        auto [ast, parse_diags] = parse(std::move(tokens), "const _ = 42");
        CHECK(!parse_diags.empty());
    }

    SECTION("cannot use underscore as function name") {
        auto [tokens, lex_diags] = lex("fn _(x) -> x");
        auto [ast, parse_diags] = parse(std::move(tokens), "fn _(x) -> x");
        CHECK(!parse_diags.empty());
    }

    SECTION("underscore in match is valid wildcard") {
        auto ast = parse_ok("match(x) { _: 0 }");
        REQUIRE(ast.valid());
    }
}

// ============================================================================
// Record and field access tests
// ============================================================================

TEST_CASE("Parser record literals", "[parser][records]") {
    SECTION("simple record literal") {
        auto ast = parse_ok("{x: 1, y: 2}");
        NodeIndex root = ast.root;
        NodeIndex child = ast.arena[root].first_child;
        REQUIRE(child != NULL_NODE);
        REQUIRE(ast.arena[child].type == NodeType::RecordLit);

        // Check fields
        NodeIndex field1 = ast.arena[child].first_child;
        REQUIRE(field1 != NULL_NODE);
        REQUIRE(ast.arena[field1].type == NodeType::Argument);
        REQUIRE(std::holds_alternative<Node::RecordFieldData>(ast.arena[field1].data));
        CHECK(ast.arena[field1].as_record_field().name == "x");
        CHECK_FALSE(ast.arena[field1].as_record_field().is_shorthand);

        NodeIndex field2 = ast.arena[field1].next_sibling;
        REQUIRE(field2 != NULL_NODE);
        CHECK(ast.arena[field2].as_record_field().name == "y");
    }

    SECTION("empty record literal") {
        auto ast = parse_ok("{}");
        NodeIndex root = ast.root;
        NodeIndex child = ast.arena[root].first_child;
        REQUIRE(child != NULL_NODE);
        REQUIRE(ast.arena[child].type == NodeType::RecordLit);
        CHECK(ast.arena[child].first_child == NULL_NODE);
    }

    SECTION("shorthand field syntax") {
        auto ast = parse_ok(R"(
            x = 1
            y = 2
            {x, y}
        )");
        NodeIndex root = ast.root;

        // Find the record literal (third statement)
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex stmt2 = ast.arena[stmt1].next_sibling;
        NodeIndex stmt3 = ast.arena[stmt2].next_sibling;
        REQUIRE(ast.arena[stmt3].type == NodeType::RecordLit);

        NodeIndex field1 = ast.arena[stmt3].first_child;
        REQUIRE(field1 != NULL_NODE);
        CHECK(ast.arena[field1].as_record_field().name == "x");
        CHECK(ast.arena[field1].as_record_field().is_shorthand);
    }

    SECTION("mixed shorthand and explicit") {
        auto ast = parse_ok(R"(
            x = 1
            {x, y: 2}
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex record = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[record].type == NodeType::RecordLit);

        NodeIndex field1 = ast.arena[record].first_child;
        NodeIndex field2 = ast.arena[field1].next_sibling;

        CHECK(ast.arena[field1].as_record_field().is_shorthand);
        CHECK_FALSE(ast.arena[field2].as_record_field().is_shorthand);
    }

    SECTION("trailing comma allowed") {
        auto ast = parse_ok("{x: 1, y: 2,}");
        NodeIndex root = ast.root;
        NodeIndex child = ast.arena[root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::RecordLit);
        CHECK(ast.arena.child_count(child) == 2);
    }
}

TEST_CASE("Parser field access", "[parser][records]") {
    SECTION("simple field access") {
        auto ast = parse_ok(R"(
            pos = {x: 1, y: 2}
            pos.x
        )");
        NodeIndex root = ast.root;
        NodeIndex assign = ast.arena[root].first_child;
        NodeIndex access = ast.arena[assign].next_sibling;

        REQUIRE(access != NULL_NODE);
        REQUIRE(ast.arena[access].type == NodeType::FieldAccess);
        CHECK(ast.arena[access].as_field_access().field_name == "x");

        // First child is the identifier 'pos'
        NodeIndex expr = ast.arena[access].first_child;
        REQUIRE(expr != NULL_NODE);
        REQUIRE(ast.arena[expr].type == NodeType::Identifier);
        CHECK(ident_text(ast.arena[expr].as_identifier()) == "pos");
    }

    SECTION("chained field access") {
        auto ast = parse_ok(R"(
            obj = {inner: {val: 42}}
            obj.inner.val
        )");
        NodeIndex root = ast.root;
        NodeIndex assign = ast.arena[root].first_child;
        NodeIndex access1 = ast.arena[assign].next_sibling;

        REQUIRE(access1 != NULL_NODE);
        REQUIRE(ast.arena[access1].type == NodeType::FieldAccess);
        CHECK(ast.arena[access1].as_field_access().field_name == "val");

        // First child should be another FieldAccess
        NodeIndex access2 = ast.arena[access1].first_child;
        REQUIRE(access2 != NULL_NODE);
        REQUIRE(ast.arena[access2].type == NodeType::FieldAccess);
        CHECK(ast.arena[access2].as_field_access().field_name == "inner");
    }

    SECTION("field access vs method call") {
        auto ast = parse_ok(R"(
            obj.field
            obj.method()
        )");
        NodeIndex root = ast.root;
        NodeIndex field = ast.arena[root].first_child;
        NodeIndex method = ast.arena[field].next_sibling;

        REQUIRE(ast.arena[field].type == NodeType::FieldAccess);
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
    }
}

TEST_CASE("Parser hole field access", "[parser][records]") {
    SECTION("hole with field") {
        auto ast = parse_ok("n\"c4\" |> %.freq");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        // RHS of pipe is the hole with field
        NodeIndex lhs = ast.arena[pipe].first_child;
        NodeIndex rhs = ast.arena[lhs].next_sibling;

        REQUIRE(ast.arena[rhs].type == NodeType::Hole);
        REQUIRE(std::holds_alternative<Node::HoleData>(ast.arena[rhs].data));
        auto& hole_data = ast.arena[rhs].as_hole();
        REQUIRE(hole_data.field_name.has_value());
        CHECK(hole_data.field_name.value() == "freq");
    }

    SECTION("bare hole has no field") {
        auto ast = parse_ok("1 |> %");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        NodeIndex lhs = ast.arena[pipe].first_child;
        NodeIndex rhs = ast.arena[lhs].next_sibling;

        REQUIRE(ast.arena[rhs].type == NodeType::Hole);
        auto& hole_data = ast.arena[rhs].as_hole();
        CHECK_FALSE(hole_data.field_name.has_value());
    }
}

TEST_CASE("Parser >> and @ aliases", "[parser]") {
    SECTION(">> as pipe alias") {
        auto ast = parse_ok("saw(440) >> lp(%, 1000)");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        NodeIndex lhs = ast.arena[pipe].first_child;
        REQUIRE(ast.arena[lhs].type == NodeType::Call);
        NodeIndex rhs = ast.arena[lhs].next_sibling;
        REQUIRE(ast.arena[rhs].type == NodeType::Call);
    }

    SECTION("@ as hole alias") {
        auto ast = parse_ok("1 |> @ + 2");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        // RHS is desugared to add(@, 2) Call
        NodeIndex lhs = ast.arena[pipe].first_child;
        NodeIndex rhs = ast.arena[lhs].next_sibling;
        REQUIRE(ast.arena[rhs].type == NodeType::Call);
        CHECK(ident_text(ast.arena[rhs].as_identifier()) == "add");
    }

    SECTION("@ with field access") {
        auto ast = parse_ok("n\"c4\" >> @.freq");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        NodeIndex lhs = ast.arena[pipe].first_child;
        NodeIndex rhs = ast.arena[lhs].next_sibling;
        REQUIRE(ast.arena[rhs].type == NodeType::Hole);
        auto& hole_data = ast.arena[rhs].as_hole();
        REQUIRE(hole_data.field_name.has_value());
        CHECK(hole_data.field_name.value() == "freq");
    }

    SECTION(">> and @ together in chain") {
        auto ast = parse_ok("saw(440) >> lp(@, 1000) >> @ * 0.5");
        NodeIndex root = ast.root;
        NodeIndex outer_pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[outer_pipe].type == NodeType::Pipe);
    }
}

// Helper: pull the hole node out of a `pat(...) |> <hole-expr>` program.
// Returns NULL_NODE if the RHS isn't a Hole.
static NodeIndex hole_rhs_of_pipe(const Ast& ast) {
    NodeIndex pipe = ast.arena[ast.root].first_child;
    if (ast.arena[pipe].type != NodeType::Pipe) return NULL_NODE;
    NodeIndex lhs = ast.arena[pipe].first_child;
    return ast.arena[lhs].next_sibling;
}

TEST_CASE("Parser hole-field dotless shorthand", "[parser][hole-shorthand]") {
    SECTION("@field parses identical to @.field") {
        auto dotless = parse_ok("n\"c4\" |> @freq");
        auto dotted  = parse_ok("n\"c4\" |> @.freq");

        NodeIndex h_less = hole_rhs_of_pipe(dotless);
        NodeIndex h_dot  = hole_rhs_of_pipe(dotted);
        REQUIRE(dotless.arena[h_less].type == NodeType::Hole);
        REQUIRE(dotted.arena[h_dot].type == NodeType::Hole);

        auto& less_data = dotless.arena[h_less].as_hole();
        auto& dot_data  = dotted.arena[h_dot].as_hole();
        REQUIRE(less_data.field_name.has_value());
        REQUIRE(dot_data.field_name.has_value());
        CHECK(less_data.field_name.value() == "freq");
        CHECK(dot_data.field_name.value() == "freq");
    }

    SECTION("%field works as legacy alias") {
        auto ast = parse_ok("n\"c4\" |> %freq");
        NodeIndex h = hole_rhs_of_pipe(ast);
        REQUIRE(ast.arena[h].type == NodeType::Hole);
        auto& d = ast.arena[h].as_hole();
        REQUIRE(d.field_name.has_value());
        CHECK(d.field_name.value() == "freq");
    }

    SECTION("aliases (@v, @t, @f) parse as field names") {
        auto ast = parse_ok("n\"c4\" |> @v");
        NodeIndex h = hole_rhs_of_pipe(ast);
        auto& d = ast.arena[h].as_hole();
        REQUIRE(d.field_name.has_value());
        CHECK(d.field_name.value() == "v");
    }

    SECTION("keyword `as` accepted as adjacent field name") {
        auto ast = parse_ok("n\"c4\" |> @as");
        NodeIndex h = hole_rhs_of_pipe(ast);
        REQUIRE(ast.arena[h].type == NodeType::Hole);
        auto& d = ast.arena[h].as_hole();
        REQUIRE(d.field_name.has_value());
        CHECK(d.field_name.value() == "as");
    }

    SECTION("keyword `match` accepted as adjacent field name") {
        auto ast = parse_ok("n\"c4\" |> @match");
        NodeIndex h = hole_rhs_of_pipe(ast);
        auto& d = ast.arena[h].as_hole();
        REQUIRE(d.field_name.has_value());
        CHECK(d.field_name.value() == "match");
    }

    SECTION("whitespace defeats shorthand (`@ as e` keeps pipe binding)") {
        // `@ as e` must parse `@` as a bare hole (NOT field "as") and treat
        // `as e` as the pipe-binding suffix. The exact tree shape depends
        // on precedence wrapping; the load-bearing invariant is that no
        // Hole node in the tree has field_name == "as".
        auto ast = parse_ok("n\"c4\" |> @ as e |> e + 1");

        bool found_bare_hole = false;
        bool found_as_field  = false;
        for (NodeIndex i = 0; i < (NodeIndex)ast.arena.size(); ++i) {
            if (ast.arena[i].type != NodeType::Hole) continue;
            auto& d = ast.arena[i].as_hole();
            if (!d.field_name.has_value()) {
                found_bare_hole = true;
            } else if (d.field_name.value() == "as") {
                found_as_field = true;
            }
        }
        CHECK(found_bare_hole);
        CHECK_FALSE(found_as_field);

        // The `as e` binding must have produced a PipeBinding{e} somewhere.
        bool found_pipe_binding_e = false;
        for (NodeIndex i = 0; i < (NodeIndex)ast.arena.size(); ++i) {
            if (ast.arena[i].type != NodeType::PipeBinding) continue;
            if (ast.arena[i].as_pipe_binding().binding_name == "e") {
                found_pipe_binding_e = true;
                break;
            }
        }
        CHECK(found_pipe_binding_e);
    }

    SECTION("chained @foo.bar parses as (@foo).bar") {
        auto ast = parse_ok("n\"c4\" |> @osc.freq");
        NodeIndex rhs = hole_rhs_of_pipe(ast);
        REQUIRE(ast.arena[rhs].type == NodeType::FieldAccess);

        // First child of FieldAccess is the receiver — must be a Hole
        // with field_name = "osc".
        NodeIndex recv = ast.arena[rhs].first_child;
        REQUIRE(ast.arena[recv].type == NodeType::Hole);
        auto& hd = ast.arena[recv].as_hole();
        REQUIRE(hd.field_name.has_value());
        CHECK(hd.field_name.value() == "osc");
    }

    SECTION("@method() emits E108 with hint") {
        auto [tokens, lex_diags] = lex("osc(\"saw\", 440) |> @lp(800)");
        REQUIRE(lex_diags.empty());
        auto [ast, diags] = parse(std::move(tokens),
                                   "osc(\"saw\", 440) |> @lp(800)");

        bool found_e108 = false;
        for (const auto& d : diags) {
            if (d.code == "E108") {
                found_e108 = true;
                CHECK(d.message.find("@.lp") != std::string::npos);
            }
        }
        CHECK(found_e108);
    }

    SECTION("bare hole at end of expression is unaffected") {
        auto ast = parse_ok("saw(440) |> lp(@, 1000)");
        // The `@` inside lp's arg list is bare — no adjacency target
        // (next token is `,`).
        NodeIndex pipe = ast.arena[ast.root].first_child;
        NodeIndex rhs = ast.arena[ast.arena[pipe].first_child].next_sibling;
        REQUIRE(ast.arena[rhs].type == NodeType::Call);
        // Don't deep-verify the @ AST — just confirm the program parses.
    }

    SECTION("dotted form still parses (back-compat)") {
        auto ast = parse_ok("n\"c4\" |> @.freq * @.vel");
        // Smoke test: no errors, AST exists. Detailed equivalence is
        // checked by the first SECTION.
        REQUIRE(ast.valid());
    }

    SECTION("W201 only fires under --strict") {
        std::string_view src = "n\"c4\" |> @.freq";

        // Default: no W201.
        {
            auto [tokens, _ld] = lex(src);
            auto [_ast, diags] = parse(std::move(tokens), src);
            for (const auto& d : diags) {
                CHECK(d.code != "W201");
            }
        }

        // Under strict: W201 fires with a suggestion mentioning `@freq`.
        {
            auto [tokens, _ld] = lex(src);
            auto [_ast, diags] = parse(std::move(tokens), src, "<input>", true);
            bool found = false;
            for (const auto& d : diags) {
                if (d.code == "W201") {
                    found = true;
                    CHECK(d.severity == Severity::Warning);
                    CHECK(d.message.find("@freq") != std::string::npos);
                }
            }
            CHECK(found);
        }
    }

    SECTION("W201 does not fire for the new dotless form even under --strict") {
        std::string_view src = "n\"c4\" |> @freq";
        auto [tokens, _ld] = lex(src);
        auto [_ast, diags] = parse(std::move(tokens), src, "<input>", true);
        for (const auto& d : diags) {
            CHECK(d.code != "W201");
        }
    }
}

TEST_CASE("Parser pipe binding", "[parser][records]") {
    SECTION("simple as binding") {
        // `sig` is now reserved (Phase 2). Use `s` as binding name instead.
        auto ast = parse_ok("osc(\"sin\", 440) as s |> lp(%, 1000)");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        // LHS should be a PipeBinding
        NodeIndex lhs = ast.arena[pipe].first_child;
        REQUIRE(ast.arena[lhs].type == NodeType::PipeBinding);
        CHECK(ast.arena[lhs].as_pipe_binding().binding_name == "s");

        // First child of PipeBinding is the bound expression
        NodeIndex expr = ast.arena[lhs].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
    }

    SECTION("binding used multiple times") {
        auto ast = parse_ok("1 as x |> x + x");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;

        NodeIndex lhs = ast.arena[pipe].first_child;
        REQUIRE(ast.arena[lhs].type == NodeType::PipeBinding);
        CHECK(ast.arena[lhs].as_pipe_binding().binding_name == "x");
    }
}

// =============================================================================
// Parser: Record spreading
// =============================================================================

TEST_CASE("Parser record spreading", "[parser][records]") {
    SECTION("spread with override") {
        auto ast = parse_ok(R"(
            base = {freq: 440, vel: 0.8}
            {..base, freq: 880}
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex record = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[record].type == NodeType::RecordLit);
        REQUIRE(std::holds_alternative<Node::RecordLitData>(ast.arena[record].data));
        CHECK(ast.arena[record].as_record_lit().spread_source != NULL_NODE);

        // Should have one explicit field (freq: 880)
        NodeIndex field = ast.arena[record].first_child;
        REQUIRE(field != NULL_NODE);
        CHECK(ast.arena[field].as_record_field().name == "freq");
    }

    SECTION("spread only - no explicit fields") {
        auto ast = parse_ok(R"(
            base = {freq: 440}
            {..base}
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex record = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[record].type == NodeType::RecordLit);
        CHECK(ast.arena[record].as_record_lit().spread_source != NULL_NODE);
        // No explicit fields
        CHECK(ast.arena[record].first_child == NULL_NODE);
    }

    SECTION("spread with new field") {
        auto ast = parse_ok(R"(
            base = {freq: 440}
            {..base, pan: 0.5}
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex record = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[record].type == NodeType::RecordLit);
        CHECK(ast.arena[record].as_record_lit().spread_source != NULL_NODE);

        NodeIndex field = ast.arena[record].first_child;
        REQUIRE(field != NULL_NODE);
        CHECK(ast.arena[field].as_record_field().name == "pan");
    }

    SECTION("inline spread source") {
        auto ast = parse_ok("{..{freq: 440}, vel: 0.8}");
        NodeIndex root = ast.root;
        NodeIndex record = ast.arena[root].first_child;
        REQUIRE(ast.arena[record].type == NodeType::RecordLit);
        CHECK(ast.arena[record].as_record_lit().spread_source != NULL_NODE);
    }

    SECTION("record without spread has NULL spread_source") {
        auto ast = parse_ok("{x: 1, y: 2}");
        NodeIndex root = ast.root;
        NodeIndex record = ast.arena[root].first_child;
        REQUIRE(ast.arena[record].type == NodeType::RecordLit);
        REQUIRE(std::holds_alternative<Node::RecordLitData>(ast.arena[record].data));
        CHECK(ast.arena[record].as_record_lit().spread_source == NULL_NODE);
    }
}

// =============================================================================
// Parser: Argument and array spread (..expr)
// =============================================================================

TEST_CASE("Parser spread arguments", "[parser][spread]") {
    SECTION("record-style spread arg in call") {
        auto ast = parse_ok(R"(
            r = {a: 1, b: 2}
            f(..r)
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex call = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        NodeIndex arg = ast.arena[call].first_child;
        REQUIRE(arg != NULL_NODE);
        REQUIRE(ast.arena[arg].type == NodeType::Argument);
        REQUIRE(std::holds_alternative<Node::ArgumentData>(ast.arena[arg].data));
        const auto& adata = ast.arena[arg].as_argument();
        CHECK_FALSE(adata.name.has_value());
        CHECK(adata.spread_source != NULL_NODE);
        // Spread source should be the identifier 'r'
        CHECK(ast.arena[adata.spread_source].type == NodeType::Identifier);
        // No child on the spread arg (expression hung off spread_source instead)
        CHECK(ast.arena[arg].first_child == NULL_NODE);
        // Only one arg
        CHECK(ast.arena[arg].next_sibling == NULL_NODE);
    }

    SECTION("array-style spread arg in call") {
        // `arr` is now reserved (Phase 2). Use `xs` as variable name instead.
        auto ast = parse_ok(R"(
            xs = [1, 2, 3]
            f(..xs)
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex call = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        NodeIndex arg = ast.arena[call].first_child;
        REQUIRE(arg != NULL_NODE);
        REQUIRE(ast.arena[arg].type == NodeType::Argument);
        const auto& adata = ast.arena[arg].as_argument();
        CHECK_FALSE(adata.name.has_value());
        CHECK(adata.spread_source != NULL_NODE);
        CHECK(ast.arena[adata.spread_source].type == NodeType::Identifier);
    }

    SECTION("inline record spread") {
        auto ast = parse_ok("f(..{a: 1, b: 2})");
        NodeIndex root = ast.root;
        NodeIndex call = ast.arena[root].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        NodeIndex arg = ast.arena[call].first_child;
        REQUIRE(arg != NULL_NODE);
        const auto& adata = ast.arena[arg].as_argument();
        CHECK(adata.spread_source != NULL_NODE);
        CHECK(ast.arena[adata.spread_source].type == NodeType::RecordLit);
    }

    SECTION("positional + spread + named arguments mixed") {
        auto ast = parse_ok(R"(
            r = {b: 2}
            f(1, ..r, c: 3)
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex call = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        // arg 1: positional 1
        NodeIndex arg1 = ast.arena[call].first_child;
        REQUIRE(arg1 != NULL_NODE);
        const auto& a1 = ast.arena[arg1].as_argument();
        CHECK_FALSE(a1.name.has_value());
        CHECK(a1.spread_source == NULL_NODE);

        // arg 2: spread
        NodeIndex arg2 = ast.arena[arg1].next_sibling;
        REQUIRE(arg2 != NULL_NODE);
        const auto& a2 = ast.arena[arg2].as_argument();
        CHECK_FALSE(a2.name.has_value());
        CHECK(a2.spread_source != NULL_NODE);

        // arg 3: named c: 3
        NodeIndex arg3 = ast.arena[arg2].next_sibling;
        REQUIRE(arg3 != NULL_NODE);
        const auto& a3 = ast.arena[arg3].as_argument();
        REQUIRE(a3.name.has_value());
        CHECK(*a3.name == "c");
        CHECK(a3.spread_source == NULL_NODE);

        CHECK(ast.arena[arg3].next_sibling == NULL_NODE);
    }

    SECTION("multiple spreads in one call") {
        auto ast = parse_ok(R"(
            r1 = {a: 1}
            r2 = {b: 2}
            f(..r1, ..r2)
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex stmt2 = ast.arena[stmt1].next_sibling;
        NodeIndex call = ast.arena[stmt2].next_sibling;
        REQUIRE(ast.arena[call].type == NodeType::Call);

        NodeIndex arg1 = ast.arena[call].first_child;
        NodeIndex arg2 = ast.arena[arg1].next_sibling;
        CHECK(ast.arena[arg1].as_argument().spread_source != NULL_NODE);
        CHECK(ast.arena[arg2].as_argument().spread_source != NULL_NODE);
    }

    SECTION("no spread - regular positional has NULL spread_source") {
        auto ast = parse_ok("f(1, 2)");
        NodeIndex root = ast.root;
        NodeIndex call = ast.arena[root].first_child;
        NodeIndex arg1 = ast.arena[call].first_child;
        CHECK(ast.arena[arg1].as_argument().spread_source == NULL_NODE);
    }

    SECTION("no spread - named arg has NULL spread_source") {
        auto ast = parse_ok("f(x: 1)");
        NodeIndex root = ast.root;
        NodeIndex call = ast.arena[root].first_child;
        NodeIndex arg = ast.arena[call].first_child;
        const auto& a = ast.arena[arg].as_argument();
        REQUIRE(a.name.has_value());
        CHECK(*a.name == "x");
        CHECK(a.spread_source == NULL_NODE);
    }
}

TEST_CASE("Parser array literal spread", "[parser][spread][array]") {
    // Note: tests use `b = [..a, ...]` (assignment) rather than bare expressions
    // because a bare `[...]` following another `]` would be parsed as indexing.

    SECTION("spread followed by literal element") {
        auto ast = parse_ok(R"(
            a = [1, 2]
            b = [..a, 3]
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex stmt2 = ast.arena[stmt1].next_sibling;
        REQUIRE(ast.arena[stmt2].type == NodeType::Assignment);
        NodeIndex arr = ast.arena[stmt2].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);

        // Element 1: spread wrapper Argument
        NodeIndex el1 = ast.arena[arr].first_child;
        REQUIRE(el1 != NULL_NODE);
        REQUIRE(ast.arena[el1].type == NodeType::Argument);
        const auto& a1 = ast.arena[el1].as_argument();
        CHECK(a1.spread_source != NULL_NODE);
        CHECK(ast.arena[a1.spread_source].type == NodeType::Identifier);

        // Element 2: bare number literal 3
        NodeIndex el2 = ast.arena[el1].next_sibling;
        REQUIRE(el2 != NULL_NODE);
        CHECK(ast.arena[el2].type == NodeType::NumberLit);
    }

    SECTION("multiple spreads in array literal") {
        auto ast = parse_ok(R"(
            a = [1, 2]
            b = [3, 4]
            c = [..a, ..b]
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex stmt2 = ast.arena[stmt1].next_sibling;
        NodeIndex stmt3 = ast.arena[stmt2].next_sibling;
        REQUIRE(ast.arena[stmt3].type == NodeType::Assignment);
        NodeIndex arr = ast.arena[stmt3].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);

        NodeIndex el1 = ast.arena[arr].first_child;
        REQUIRE(el1 != NULL_NODE);
        CHECK(ast.arena[el1].as_argument().spread_source != NULL_NODE);

        NodeIndex el2 = ast.arena[el1].next_sibling;
        REQUIRE(el2 != NULL_NODE);
        CHECK(ast.arena[el2].as_argument().spread_source != NULL_NODE);
    }

    SECTION("inline array spread") {
        auto ast = parse_ok("[..[1, 2], 3]");
        NodeIndex root = ast.root;
        NodeIndex arr = ast.arena[root].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);

        NodeIndex el1 = ast.arena[arr].first_child;
        REQUIRE(el1 != NULL_NODE);
        const auto& a1 = ast.arena[el1].as_argument();
        CHECK(a1.spread_source != NULL_NODE);
        CHECK(ast.arena[a1.spread_source].type == NodeType::ArrayLit);
    }

    SECTION("regular array still has bare children") {
        auto ast = parse_ok("[1, 2, 3]");
        NodeIndex root = ast.root;
        NodeIndex arr = ast.arena[root].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);

        NodeIndex el1 = ast.arena[arr].first_child;
        REQUIRE(el1 != NULL_NODE);
        // Plain number literal — not an Argument wrapper
        CHECK(ast.arena[el1].type == NodeType::NumberLit);
    }

    SECTION("spread at end of array") {
        auto ast = parse_ok(R"(
            a = [2, 3]
            b = [1, ..a]
        )");
        NodeIndex root = ast.root;
        NodeIndex stmt1 = ast.arena[root].first_child;
        NodeIndex stmt2 = ast.arena[stmt1].next_sibling;
        NodeIndex arr = ast.arena[stmt2].first_child;
        REQUIRE(ast.arena[arr].type == NodeType::ArrayLit);

        NodeIndex el1 = ast.arena[arr].first_child;
        CHECK(ast.arena[el1].type == NodeType::NumberLit);
        NodeIndex el2 = ast.arena[el1].next_sibling;
        REQUIRE(el2 != NULL_NODE);
        REQUIRE(ast.arena[el2].type == NodeType::Argument);
        CHECK(ast.arena[el2].as_argument().spread_source != NULL_NODE);
    }
}

// =============================================================================
// Parser: Expression defaults
// =============================================================================

TEST_CASE("Parser expression defaults", "[parser][fn]") {
    SECTION("arithmetic expression default") {
        auto ast = parse_ok("fn f(x, cut = 440 * 2) -> x");
        NodeIndex root = ast.root;
        NodeIndex fn_def = ast.arena[root].first_child;
        REQUIRE(ast.arena[fn_def].type == NodeType::FunctionDef);

        // Second param should have ClosureParamData (expression default)
        NodeIndex param1 = ast.arena[fn_def].first_child;
        NodeIndex param2 = ast.arena[param1].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param2].data));
        const auto& cp = ast.arena[param2].as_closure_param();
        CHECK(cp.name == "cut");
        // Expression default: no numeric default_value
        CHECK_FALSE(cp.default_value.has_value());
        // But the param node should have a child (the expression AST)
        CHECK(ast.arena[param2].first_child != NULL_NODE);
    }

    SECTION("function call expression default") {
        auto ast = parse_ok("const fn mtof(n) -> 440 * 2 ^ ((n - 69) / 12)\nfn f(x, freq = mtof(60)) -> x");
        NodeIndex root = ast.root;
        NodeIndex fn_def1 = ast.arena[root].first_child;
        NodeIndex fn_def2 = ast.arena[fn_def1].next_sibling;
        REQUIRE(ast.arena[fn_def2].type == NodeType::FunctionDef);

        NodeIndex param1 = ast.arena[fn_def2].first_child;
        NodeIndex param2 = ast.arena[param1].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param2].data));
        CHECK(ast.arena[param2].as_closure_param().name == "freq");
        CHECK(ast.arena[param2].first_child != NULL_NODE);
    }

    SECTION("closure with expression default") {
        auto ast = parse_ok("f = (x, cut = 440 * 2) -> x");
        NodeIndex root = ast.root;
        NodeIndex assign = ast.arena[root].first_child;
        // The RHS is a Closure
        NodeIndex closure = ast.arena[assign].first_child;
        REQUIRE(closure != NULL_NODE);
        REQUIRE(ast.arena[closure].type == NodeType::Closure);

        // Params are before the body
        NodeIndex param1 = ast.arena[closure].first_child;
        NodeIndex param2 = ast.arena[param1].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param2].data));
        CHECK(ast.arena[param2].as_closure_param().name == "cut");
        CHECK(ast.arena[param2].first_child != NULL_NODE);
    }

    SECTION("simple literal defaults still work") {
        auto ast = parse_ok("fn f(x, freq = 440) -> x");
        NodeIndex root = ast.root;
        NodeIndex fn_def = ast.arena[root].first_child;
        NodeIndex param1 = ast.arena[fn_def].first_child;
        NodeIndex param2 = ast.arena[param1].next_sibling;
        REQUIRE(std::holds_alternative<Node::ClosureParamData>(ast.arena[param2].data));
        const auto& cp = ast.arena[param2].as_closure_param();
        CHECK(cp.name == "freq");
        REQUIRE(cp.default_value.has_value());
        CHECK_THAT(*cp.default_value, WithinRel(440.0));
    }
}
