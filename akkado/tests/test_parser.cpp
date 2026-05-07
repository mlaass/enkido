#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"

using namespace akkado;
using Catch::Matchers::WithinRel;

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
        CHECK(ast.arena[child].as_identifier() == "foo");
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
        CHECK(ast.arena[child].as_identifier() == "add");

        // Should have two argument children
        CHECK(ast.arena.child_count(child) == 2);
    }

    SECTION("subtraction") {
        auto ast = parse_ok("5 - 3");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ast.arena[child].as_identifier() == "sub");
    }

    SECTION("multiplication") {
        auto ast = parse_ok("2 * 3");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ast.arena[child].as_identifier() == "mul");
    }

    SECTION("division") {
        auto ast = parse_ok("10 / 2");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ast.arena[child].as_identifier() == "div");
    }

    SECTION("power") {
        auto ast = parse_ok("2 ^ 3");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ast.arena[child].as_identifier() == "pow");
    }

    SECTION("precedence: mul before add") {
        // 1 + 2 * 3 should parse as add(1, mul(2, 3))
        auto ast = parse_ok("1 + 2 * 3");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ast.arena[expr].as_identifier() == "add");

        // Second argument should be mul
        NodeIndex first_arg = ast.arena[expr].first_child;
        NodeIndex second_arg = ast.arena[first_arg].next_sibling;
        REQUIRE(second_arg != NULL_NODE);

        // The argument node contains the actual expression
        NodeIndex mul_expr = ast.arena[second_arg].first_child;
        REQUIRE(ast.arena[mul_expr].type == NodeType::Call);
        CHECK(ast.arena[mul_expr].as_identifier() == "mul");
    }

    SECTION("left associativity") {
        // 1 - 2 - 3 should parse as sub(sub(1, 2), 3)
        auto ast = parse_ok("1 - 2 - 3");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ast.arena[expr].as_identifier() == "sub");

        // First argument should be another sub
        NodeIndex first_arg = ast.arena[expr].first_child;
        NodeIndex inner_sub = ast.arena[first_arg].first_child;
        REQUIRE(ast.arena[inner_sub].type == NodeType::Call);
        CHECK(ast.arena[inner_sub].as_identifier() == "sub");
    }
}

TEST_CASE("Parser function calls", "[parser]") {
    SECTION("no arguments") {
        auto ast = parse_ok("foo()");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ast.arena[child].as_identifier() == "foo");
        CHECK(ast.arena.child_count(child) == 0);
    }

    SECTION("single argument") {
        auto ast = parse_ok("sin(440)");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ast.arena[child].as_identifier() == "sin");
        CHECK(ast.arena.child_count(child) == 1);
    }

    SECTION("multiple arguments") {
        auto ast = parse_ok("lp(x, 1000, 0.7)");
        NodeIndex child = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[child].type == NodeType::Call);
        CHECK(ast.arena[child].as_identifier() == "lp");
        CHECK(ast.arena.child_count(child) == 3);
    }

    SECTION("named arguments") {
        auto ast = parse_ok("svflp(in: x, cut: 800, q: 0.5)");
        NodeIndex call = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);
        CHECK(ast.arena[call].as_identifier() == "svflp");

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
        CHECK(ast.arena[outer].as_identifier() == "f");

        // The argument's child should be another call
        NodeIndex arg = ast.arena[outer].first_child;
        NodeIndex inner = ast.arena[arg].first_child;
        REQUIRE(ast.arena[inner].type == NodeType::Call);
        CHECK(ast.arena[inner].as_identifier() == "g");
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
        CHECK(ast.arena[second].as_identifier() == "mul");
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
        CHECK(ast.arena[param].as_identifier() == "x");

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
        CHECK(ast.arena[body].as_identifier() == "add");
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
        CHECK(ast.arena[assign].as_identifier() == "x");

        NodeIndex value = ast.arena[assign].first_child;
        REQUIRE(ast.arena[value].type == NodeType::NumberLit);
    }

    SECTION("assignment with expression") {
        auto ast = parse_ok("bpm = 120");
        NodeIndex assign = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[assign].type == NodeType::Assignment);
        CHECK(ast.arena[assign].as_identifier() == "bpm");
    }

    SECTION("assignment with pipe") {
        auto ast = parse_ok("sig = saw(440) |> lp(%, 1000)");
        NodeIndex assign = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[assign].type == NodeType::Assignment);

        NodeIndex value = ast.arena[assign].first_child;
        CHECK(ast.arena[value].type == NodeType::Pipe);
    }
}

TEST_CASE("Parser mini-notation", "[parser]") {
    SECTION("simple pat") {
        auto ast = parse_ok("pat(\"bd sd\")");
        NodeIndex mini = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[mini].type == NodeType::MiniLiteral);

        // First child is the parsed MiniPattern (not StringLit anymore)
        NodeIndex pattern = ast.arena[mini].first_child;
        REQUIRE(ast.arena[pattern].type == NodeType::MiniPattern);
        // MiniPattern should have 2 sample atoms: "bd" and "sd"
        CHECK(ast.arena.child_count(pattern) == 2);
    }

    SECTION("pat with closure") {
        auto ast = parse_ok("pat(\"c4 e4 g4\", (t, v, p) -> saw(p))");
        NodeIndex mini = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[mini].type == NodeType::MiniLiteral);

        // Should have 2 children: MiniPattern and closure
        CHECK(ast.arena.child_count(mini) == 2);

        NodeIndex pattern = ast.arena[mini].first_child;
        CHECK(ast.arena[pattern].type == NodeType::MiniPattern);

        NodeIndex closure = ast.arena[pattern].next_sibling;
        CHECK(ast.arena[closure].type == NodeType::Closure);
    }
}

TEST_CASE("Parser complex expressions", "[parser]") {
    SECTION("math with multiple operators") {
        auto ast = parse_ok("400 + 300 * co");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ast.arena[expr].as_identifier() == "add");
    }

    SECTION("parenthesized expression") {
        auto ast = parse_ok("(1 + 2) * 3");
        NodeIndex expr = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[expr].type == NodeType::Call);
        CHECK(ast.arena[expr].as_identifier() == "mul");

        // First arg should be add
        NodeIndex first_arg = ast.arena[expr].first_child;
        NodeIndex add = ast.arena[first_arg].first_child;
        REQUIRE(ast.arena[add].type == NodeType::Call);
        CHECK(ast.arena[add].as_identifier() == "add");
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
        CHECK(ast.arena[method].as_identifier() == "foo");

        // Should have receiver as first child (x)
        NodeIndex receiver = ast.arena[method].first_child;
        REQUIRE(ast.arena[receiver].type == NodeType::Identifier);
        CHECK(ast.arena[receiver].as_identifier() == "x");

        // No additional arguments
        CHECK(ast.arena[receiver].next_sibling == NULL_NODE);
    }

    SECTION("method call with arguments") {
        auto ast = parse_ok("osc.filter(1000, 0.5)");
        NodeIndex method = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
        CHECK(ast.arena[method].as_identifier() == "filter");

        // First child is receiver, then arguments
        NodeIndex receiver = ast.arena[method].first_child;
        REQUIRE(ast.arena[receiver].type == NodeType::Identifier);
        CHECK(ast.arena[receiver].as_identifier() == "osc");

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
        CHECK(ast.arena[outer].as_identifier() == "bar");

        // Receiver should be inner method call
        NodeIndex inner = ast.arena[outer].first_child;
        REQUIRE(ast.arena[inner].type == NodeType::MethodCall);
        CHECK(ast.arena[inner].as_identifier() == "foo");

        // Inner receiver should be x
        NodeIndex x = ast.arena[inner].first_child;
        REQUIRE(ast.arena[x].type == NodeType::Identifier);
        CHECK(ast.arena[x].as_identifier() == "x");
    }

    SECTION("method call on function result") {
        auto ast = parse_ok("foo(1).bar()");
        NodeIndex method = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
        CHECK(ast.arena[method].as_identifier() == "bar");

        // Receiver should be function call
        NodeIndex call = ast.arena[method].first_child;
        REQUIRE(ast.arena[call].type == NodeType::Call);
        CHECK(ast.arena[call].as_identifier() == "foo");
    }

    SECTION("method call with pipe") {
        auto ast = parse_ok("saw(440) |> %.filter(1000)");
        NodeIndex pipe = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        // Second part should be method call
        NodeIndex lhs = ast.arena[pipe].first_child;
        NodeIndex rhs = ast.arena[lhs].next_sibling;
        REQUIRE(ast.arena[rhs].type == NodeType::MethodCall);
        CHECK(ast.arena[rhs].as_identifier() == "filter");

        // Receiver should be hole
        NodeIndex receiver = ast.arena[rhs].first_child;
        CHECK(ast.arena[receiver].type == NodeType::Hole);
    }

    SECTION("method call mixed with operators") {
        auto ast = parse_ok("x.foo() + y.bar()");
        NodeIndex add = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[add].type == NodeType::Call);
        CHECK(ast.arena[add].as_identifier() == "add");

        // Both arguments should be method calls (wrapped in Argument nodes)
        NodeIndex arg1 = ast.arena[add].first_child;
        NodeIndex arg2 = ast.arena[arg1].next_sibling;

        NodeIndex method1 = ast.arena[arg1].first_child;
        NodeIndex method2 = ast.arena[arg2].first_child;

        REQUIRE(ast.arena[method1].type == NodeType::MethodCall);
        CHECK(ast.arena[method1].as_identifier() == "foo");

        REQUIRE(ast.arena[method2].type == NodeType::MethodCall);
        CHECK(ast.arena[method2].as_identifier() == "bar");
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
        CHECK(ast.arena[rhs].as_identifier() == "r");
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
            fn lp_voice(freq, {cutoff, q = 0.7}) -> osc("saw", freq)
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
            pat("c4") as {freq = 440} |> osc("sin", freq)
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

    SECTION("destructure parameter in closure does not parse cleanly") {
        // Phase 3b only enables destructure params in `fn` definitions, not
        // arrow-style closures. The closure-detection heuristic doesn't
        // recognize `{` as a param start, so the `({x,y}) -> …` form falls
        // through to grouped-expression parsing, which fails. The exact
        // error message is implementation-detail; what matters is the
        // form doesn't silently compile to something surprising.
        auto [tokens, lex_diags] = lex(R"(
            f = ({x, y}) -> x + y
        )");
        REQUIRE(lex_diags.empty());
        auto [ast, parse_diags] = parse(std::move(tokens), "src");
        bool any_error = false;
        for (const auto& d : parse_diags) {
            if (d.severity == Severity::Error) any_error = true;
        }
        CHECK(any_error);
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
        CHECK(ast.arena[elem1].as_identifier() == "add");

        REQUIRE(ast.arena[elem2].type == NodeType::Call);
        CHECK(ast.arena[elem2].as_identifier() == "foo");
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
        auto ast = parse_ok("arr = [1, 2, 3]");
        NodeIndex assign = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[assign].type == NodeType::Assignment);
        CHECK(ast.arena[assign].as_identifier() == "arr");

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
        auto ast = parse_ok("arr[0]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);
        CHECK(ast.arena.child_count(index) == 2);

        NodeIndex arr = ast.arena[index].first_child;
        NodeIndex idx_expr = ast.arena[arr].next_sibling;

        REQUIRE(ast.arena[arr].type == NodeType::Identifier);
        CHECK(ast.arena[arr].as_identifier() == "arr");

        REQUIRE(ast.arena[idx_expr].type == NodeType::NumberLit);
        CHECK_THAT(ast.arena[idx_expr].as_number(), WithinRel(0.0));
    }

    SECTION("array indexing with variable") {
        auto ast = parse_ok("arr[i]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);

        NodeIndex arr = ast.arena[index].first_child;
        NodeIndex idx_expr = ast.arena[arr].next_sibling;

        REQUIRE(ast.arena[idx_expr].type == NodeType::Identifier);
        CHECK(ast.arena[idx_expr].as_identifier() == "i");
    }

    SECTION("array indexing with expression") {
        auto ast = parse_ok("arr[i + 1]");
        NodeIndex index = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[index].type == NodeType::Index);

        NodeIndex arr = ast.arena[index].first_child;
        NodeIndex idx_expr = ast.arena[arr].next_sibling;

        REQUIRE(ast.arena[idx_expr].type == NodeType::Call);
        CHECK(ast.arena[idx_expr].as_identifier() == "add");
    }

    SECTION("chained indexing") {
        auto ast = parse_ok("arr[0][1]");
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
        CHECK(ast.arena[call].as_identifier() == "foo");
    }

    SECTION("method call on indexed value") {
        auto ast = parse_ok("arr[0].foo()");
        NodeIndex method = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[method].type == NodeType::MethodCall);
        CHECK(ast.arena[method].as_identifier() == "foo");

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
        CHECK(ast.arena[param].as_identifier() == "x");

        // Check body
        NodeIndex body = ast.arena[param].next_sibling;
        REQUIRE(ast.arena[body].type == NodeType::Call);
        CHECK(ast.arena[body].as_identifier() == "mul");
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
        CHECK(ast.arena[inner].as_identifier() == "_");
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
        CHECK(ast.arena[expr].as_identifier() == "pos");
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
        auto ast = parse_ok("pat(\"c4\") |> %.freq");
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
        CHECK(ast.arena[rhs].as_identifier() == "add");
    }

    SECTION("@ with field access") {
        auto ast = parse_ok("pat(\"c4\") >> @.freq");
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

TEST_CASE("Parser pipe binding", "[parser][records]") {
    SECTION("simple as binding") {
        auto ast = parse_ok("osc(\"sin\", 440) as sig |> lp(%, 1000)");
        NodeIndex root = ast.root;
        NodeIndex pipe = ast.arena[root].first_child;
        REQUIRE(ast.arena[pipe].type == NodeType::Pipe);

        // LHS should be a PipeBinding
        NodeIndex lhs = ast.arena[pipe].first_child;
        REQUIRE(ast.arena[lhs].type == NodeType::PipeBinding);
        CHECK(ast.arena[lhs].as_pipe_binding().binding_name == "sig");

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
        auto ast = parse_ok(R"(
            arr = [1, 2, 3]
            f(..arr)
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
