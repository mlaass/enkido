#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <akkado/lex_primitives.hpp>

using namespace akkado::lex_primitives;
using Catch::Matchers::WithinRel;

// ============================================================================
// Hardening PRD Phase 5 — shared lexing primitives [P5]
// ============================================================================

namespace {
// Test cursor over a string; CursorBase's ctor is all we need.
struct TestCursor : CursorBase {
    explicit TestCursor(std::string_view s) : CursorBase(s) {}
};
} // namespace

TEST_CASE("P5: CursorBase navigation and line/column tracking",
          "[P5][lex_primitives]") {
    TestCursor cur("ab\nc");
    CHECK(cur.peek() == 'a');
    CHECK(cur.peek_next() == 'b');
    CHECK(cur.peek_ahead(2) == '\n');
    CHECK(cur.line() == 1);
    CHECK(cur.column() == 1);

    CHECK(cur.advance() == 'a');
    CHECK(cur.column() == 2);
    CHECK(cur.match('x') == false);
    CHECK(cur.match('b') == true);

    CHECK(cur.advance() == '\n');
    CHECK(cur.line() == 2);
    CHECK(cur.column() == 1);

    cur.mark_start();
    CHECK(cur.start() == 3);
    CHECK(cur.advance() == 'c');
    CHECK(cur.slice(cur.start()) == "c");
    CHECK(cur.is_at_end());
    CHECK(cur.peek() == '\0');
    CHECK(cur.peek_next() == '\0');
    CHECK(cur.match('c') == false);  // at end
}

TEST_CASE("P5: character classifiers", "[P5][lex_primitives]") {
    STATIC_CHECK(is_digit('0'));
    STATIC_CHECK(is_digit('9'));
    STATIC_CHECK_FALSE(is_digit('a'));
    STATIC_CHECK(is_alpha('z'));
    STATIC_CHECK(is_alpha('A'));
    STATIC_CHECK(is_alpha('_'));
    STATIC_CHECK_FALSE(is_alpha('1'));
    STATIC_CHECK(is_alpha_numeric('g'));
    STATIC_CHECK(is_alpha_numeric('7'));
    STATIC_CHECK_FALSE(is_alpha_numeric('-'));
    STATIC_CHECK(is_whitespace(' '));
    STATIC_CHECK(is_whitespace('\t'));
    STATIC_CHECK(is_whitespace('\r'));
    STATIC_CHECK(is_whitespace('\n'));
    STATIC_CHECK_FALSE(is_whitespace('x'));
    STATIC_CHECK(is_pitch_letter('a'));
    STATIC_CHECK(is_pitch_letter('G'));
    STATIC_CHECK_FALSE(is_pitch_letter('h'));
    STATIC_CHECK_FALSE(is_pitch_letter('_'));
}

TEST_CASE("P5: scan_number basic forms", "[P5][lex_primitives]") {
    SECTION("integer") {
        TestCursor cur("314 rest");
        auto res = scan_number(cur, 0, {});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(314.0));
        CHECK(res.is_integer);
        CHECK(cur.peek() == ' ');
    }
    SECTION("float") {
        TestCursor cur("3.14");
        auto res = scan_number(cur, 0, {});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(3.14));
        CHECK_FALSE(res.is_integer);
    }
    SECTION("leading dot") {
        TestCursor cur(".5");
        auto res = scan_number(cur, 0, {});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(0.5));
        CHECK_FALSE(res.is_integer);
    }
    SECTION("non-greedy dot leaves trailing '.' unconsumed") {
        TestCursor cur("1.x");
        auto res = scan_number(cur, 0, {});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(1.0));
        CHECK(res.is_integer);
        CHECK(cur.peek() == '.');  // "1" scanned, ".x" left
    }
    SECTION("greedy dot consumes trailing '.'") {
        TestCursor cur("1.x");
        auto res = scan_number(cur, 0, {.greedy_dot = true});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(1.0));
        CHECK(cur.peek() == 'x');
    }
    SECTION("sign requires allow_sign") {
        TestCursor cur("-2");
        auto res = scan_number(cur, 0, {});
        CHECK_FALSE(res.ok);  // '-' not consumed, nothing scanned

        TestCursor cur2("-2");
        auto res2 = scan_number(cur2, 0, {.allow_sign = true});
        REQUIRE(res2.ok);
        CHECK_THAT(res2.value, WithinRel(-2.0));
    }
    SECTION("no digits fails") {
        TestCursor cur("abc");
        CHECK_FALSE(scan_number(cur, 0, {}).ok);
        CHECK(cur.peek() == 'a');  // nothing consumed
    }
}

TEST_CASE("P5: scan_number exponent handling", "[P5][lex_primitives]") {
    SECTION("valid exponent") {
        TestCursor cur("1e3");
        auto res = scan_number(cur, 0, {.allow_exponent = true});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(1000.0));
        CHECK_FALSE(res.is_integer);
    }
    SECTION("signed exponent") {
        TestCursor cur("2.5e-2");
        auto res = scan_number(cur, 0, {.allow_exponent = true});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(0.025));
    }
    SECTION("invalid exponent is never consumed (no cursor/column drift)") {
        TestCursor cur("5e+");
        auto res = scan_number(cur, 0, {.allow_exponent = true});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(5.0));
        CHECK(cur.peek() == 'e');
        CHECK(cur.column() == 2);  // only the '5' advanced
    }
    SECTION("exponent disabled leaves e") {
        TestCursor cur("1e3");
        auto res = scan_number(cur, 0, {});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(1.0));
        CHECK(cur.peek() == 'e');
    }
}

TEST_CASE("P5: scan_number with caller-consumed prefix", "[P5][lex_primitives]") {
    // The full Lexer dispatches after consuming the first char; scan_number
    // continues from the cursor but parses from the token start.
    SECTION("digit preconsumed") {
        TestCursor cur("42");
        cur.advance();  // Lexer consumed '4'
        auto res = scan_number(cur, 0, {.allow_exponent = true});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(42.0));
        CHECK(res.is_integer);
    }
    SECTION("leading dot preconsumed (seen_dot)") {
        TestCursor cur(".125");
        cur.advance();  // Lexer consumed '.'
        auto res = scan_number(cur, 0, {.allow_exponent = true, .seen_dot = true});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(0.125));
        CHECK_FALSE(res.is_integer);  // the preconsumed dot counts
    }
    SECTION("seen_dot suppresses a second fraction") {
        TestCursor cur(".5.5");
        cur.advance();
        auto res = scan_number(cur, 0, {.seen_dot = true});
        REQUIRE(res.ok);
        CHECK_THAT(res.value, WithinRel(0.5));
        CHECK(cur.peek() == '.');  // second ".5" left for the next token
    }
}

TEST_CASE("P5: scan_velocity_suffix", "[P5][lex_primitives]") {
    SECTION("absent — consumes nothing, returns 1.0") {
        TestCursor cur("*2");
        CHECK(scan_velocity_suffix(cur) == 1.0f);
        CHECK(cur.current() == 0);
    }
    SECTION("colon without number is not a suffix") {
        TestCursor cur(":x");
        CHECK(scan_velocity_suffix(cur) == 1.0f);
        CHECK(cur.current() == 0);
    }
    SECTION(":0.8") {
        TestCursor cur(":0.8 rest");
        CHECK_THAT(scan_velocity_suffix(cur), WithinRel(0.8f));
        CHECK(cur.peek() == ' ');
    }
    SECTION(":.7 leading-dot form") {
        TestCursor cur(":.7");
        CHECK_THAT(scan_velocity_suffix(cur), WithinRel(0.7f));
        CHECK(cur.is_at_end());
    }
    SECTION("clamped to [0, 1]") {
        TestCursor cur(":3.5");
        CHECK(scan_velocity_suffix(cur) == 1.0f);
    }
    SECTION("leading-dot form does not eat a second dot") {
        TestCursor cur(":.5.5");
        CHECK_THAT(scan_velocity_suffix(cur), WithinRel(0.5f));
        CHECK(cur.peek() == '.');
    }
}

TEST_CASE("P5: pitch_to_midi", "[P5][lex_primitives]") {
    CHECK(pitch_to_midi('c', 0, 4) == 60);   // middle C
    CHECK(pitch_to_midi('C', 0, 4) == 60);   // case-insensitive
    CHECK(pitch_to_midi('a', 0, 4) == 69);   // A4
    CHECK(pitch_to_midi('c', 1, 4) == 61);   // c#4
    CHECK(pitch_to_midi('d', -1, 4) == 61);  // db4
    CHECK(pitch_to_midi('b', 0, 3) == 59);
    CHECK(pitch_to_midi('c', 0, -2) == 0);   // clamped low
    CHECK(pitch_to_midi('g', 0, 10) == 127); // clamped high
}
