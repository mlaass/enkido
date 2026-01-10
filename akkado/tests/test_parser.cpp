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
            INFO("Parse error: " << d.message);
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
        CHECK(ast.arena[mini].as_pattern_type() == PatternType::Pat);

        // First child is the parsed MiniPattern (not StringLit anymore)
        NodeIndex pattern = ast.arena[mini].first_child;
        REQUIRE(ast.arena[pattern].type == NodeType::MiniPattern);
        // MiniPattern should have 2 sample atoms: "bd" and "sd"
        CHECK(ast.arena.child_count(pattern) == 2);
    }

    SECTION("seq with closure") {
        auto ast = parse_ok("seq(\"c4 e4 g4\", (t, v, p) -> saw(p))");
        NodeIndex mini = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[mini].type == NodeType::MiniLiteral);
        CHECK(ast.arena[mini].as_pattern_type() == PatternType::Seq);

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
        auto [tokens, lex_diags] = lex("x @ y");  // @ is not a valid operator
        auto [ast, parse_diags] = parse(std::move(tokens), "x @ y");
        // Should either lex error or parse error
        bool has_error = !lex_diags.empty() || !parse_diags.empty();
        CHECK(has_error);
    }
}

TEST_CASE("Parser post statement", "[parser]") {
    SECTION("post with closure") {
        auto ast = parse_ok("post((x) -> x)");
        NodeIndex post = ast.arena[ast.root].first_child;
        REQUIRE(ast.arena[post].type == NodeType::PostStmt);

        NodeIndex closure = ast.arena[post].first_child;
        CHECK(ast.arena[closure].type == NodeType::Closure);
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
