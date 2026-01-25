#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/lexer.hpp"

using namespace akkado;
using Catch::Matchers::WithinRel;

TEST_CASE("Lexer basic tokens", "[lexer]") {
    SECTION("empty source") {
        auto [tokens, diags] = lex("");
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].type == TokenType::Eof);
        CHECK(diags.empty());
    }

    SECTION("whitespace only") {
        auto [tokens, diags] = lex("   \t\n  \r\n  ");
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].type == TokenType::Eof);
        CHECK(diags.empty());
    }

    SECTION("single character tokens") {
        auto [tokens, diags] = lex("( ) [ ] { } , : ; % @ ~ ^ .");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 15); // 14 tokens + Eof

        CHECK(tokens[0].type == TokenType::LParen);
        CHECK(tokens[1].type == TokenType::RParen);
        CHECK(tokens[2].type == TokenType::LBracket);
        CHECK(tokens[3].type == TokenType::RBracket);
        CHECK(tokens[4].type == TokenType::LBrace);
        CHECK(tokens[5].type == TokenType::RBrace);
        CHECK(tokens[6].type == TokenType::Comma);
        CHECK(tokens[7].type == TokenType::Colon);
        CHECK(tokens[8].type == TokenType::Semicolon);
        CHECK(tokens[9].type == TokenType::Hole);
        CHECK(tokens[10].type == TokenType::At);
        CHECK(tokens[11].type == TokenType::Tilde);
        CHECK(tokens[12].type == TokenType::Caret);
        CHECK(tokens[13].type == TokenType::Dot);
    }

    SECTION("operators") {
        auto [tokens, diags] = lex("+ - * / |> = -> == != < > <= >=");
        REQUIRE(diags.empty());

        CHECK(tokens[0].type == TokenType::Plus);
        CHECK(tokens[1].type == TokenType::Minus);
        CHECK(tokens[2].type == TokenType::Star);
        CHECK(tokens[3].type == TokenType::Slash);
        CHECK(tokens[4].type == TokenType::Pipe);
        CHECK(tokens[5].type == TokenType::Equals);
        CHECK(tokens[6].type == TokenType::Arrow);
        CHECK(tokens[7].type == TokenType::EqualEqual);
        CHECK(tokens[8].type == TokenType::BangEqual);
        CHECK(tokens[9].type == TokenType::Less);
        CHECK(tokens[10].type == TokenType::Greater);
        CHECK(tokens[11].type == TokenType::LessEqual);
        CHECK(tokens[12].type == TokenType::GreaterEqual);
    }

    SECTION("logical operators") {
        auto [tokens, diags] = lex("&& || !");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);  // 3 tokens + Eof

        CHECK(tokens[0].type == TokenType::AndAnd);
        CHECK(tokens[1].type == TokenType::OrOr);
        CHECK(tokens[2].type == TokenType::Bang);
    }

    SECTION("logical operators in expressions") {
        auto [tokens, diags] = lex("a && b || !c");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 7);  // a && b || ! c + Eof

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::AndAnd);
        CHECK(tokens[2].type == TokenType::Identifier);
        CHECK(tokens[3].type == TokenType::OrOr);
        CHECK(tokens[4].type == TokenType::Bang);
        CHECK(tokens[5].type == TokenType::Identifier);
    }
}

TEST_CASE("Lexer numbers", "[lexer]") {
    SECTION("integers") {
        auto [tokens, diags] = lex("0 42 123 999");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 5);

        CHECK(tokens[0].type == TokenType::Number);
        CHECK_THAT(tokens[0].as_number(), WithinRel(0.0));

        CHECK(tokens[1].type == TokenType::Number);
        CHECK_THAT(tokens[1].as_number(), WithinRel(42.0));

        CHECK(tokens[2].type == TokenType::Number);
        CHECK_THAT(tokens[2].as_number(), WithinRel(123.0));

        CHECK(tokens[3].type == TokenType::Number);
        CHECK_THAT(tokens[3].as_number(), WithinRel(999.0));
    }

    SECTION("floating point") {
        auto [tokens, diags] = lex("3.14 0.5 123.456");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        CHECK_THAT(tokens[0].as_number(), WithinRel(3.14));
        CHECK_THAT(tokens[1].as_number(), WithinRel(0.5));
        CHECK_THAT(tokens[2].as_number(), WithinRel(123.456));
    }

    SECTION("negative numbers") {
        auto [tokens, diags] = lex("-1 -3.14");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == TokenType::Number);
        CHECK_THAT(tokens[0].as_number(), WithinRel(-1.0));

        CHECK(tokens[1].type == TokenType::Number);
        CHECK_THAT(tokens[1].as_number(), WithinRel(-3.14));
    }

    SECTION("number followed by operator") {
        auto [tokens, diags] = lex("42+3");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        CHECK(tokens[0].type == TokenType::Number);
        CHECK(tokens[1].type == TokenType::Plus);
        CHECK(tokens[2].type == TokenType::Number);
    }

    SECTION("leading decimal") {
        auto [tokens, diags] = lex(".001 .5 .123456");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        CHECK(tokens[0].type == TokenType::Number);
        CHECK_THAT(tokens[0].as_number(), WithinRel(0.001));

        CHECK(tokens[1].type == TokenType::Number);
        CHECK_THAT(tokens[1].as_number(), WithinRel(0.5));

        CHECK(tokens[2].type == TokenType::Number);
        CHECK_THAT(tokens[2].as_number(), WithinRel(0.123456));
    }

    SECTION("scientific notation") {
        auto [tokens, diags] = lex("1e3 1E3 1e-3 1e+3 2.5e10 2.5E-10");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 7);

        CHECK(tokens[0].type == TokenType::Number);
        CHECK_THAT(tokens[0].as_number(), WithinRel(1000.0));

        CHECK(tokens[1].type == TokenType::Number);
        CHECK_THAT(tokens[1].as_number(), WithinRel(1000.0));

        CHECK(tokens[2].type == TokenType::Number);
        CHECK_THAT(tokens[2].as_number(), WithinRel(0.001));

        CHECK(tokens[3].type == TokenType::Number);
        CHECK_THAT(tokens[3].as_number(), WithinRel(1000.0));

        CHECK(tokens[4].type == TokenType::Number);
        CHECK_THAT(tokens[4].as_number(), WithinRel(2.5e10));

        CHECK(tokens[5].type == TokenType::Number);
        CHECK_THAT(tokens[5].as_number(), WithinRel(2.5e-10));
    }

    SECTION("leading decimal with scientific notation") {
        auto [tokens, diags] = lex(".5e2 .001E3");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == TokenType::Number);
        CHECK_THAT(tokens[0].as_number(), WithinRel(50.0));

        CHECK(tokens[1].type == TokenType::Number);
        CHECK_THAT(tokens[1].as_number(), WithinRel(1.0));
    }

    SECTION("integer distinguished from float") {
        auto [tokens, diags] = lex("42 42.0 42e0");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        // 42 is integer
        CHECK(std::get<NumericValue>(tokens[0].value).is_integer);

        // 42.0 is not integer
        CHECK_FALSE(std::get<NumericValue>(tokens[1].value).is_integer);

        // 42e0 is not integer (has exponent)
        CHECK_FALSE(std::get<NumericValue>(tokens[2].value).is_integer);
    }
}

TEST_CASE("Lexer pitch literals", "[lexer]") {
    SECTION("basic pitches") {
        auto [tokens, diags] = lex("'c4' 'a4' 'g3'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        // C4 = 60 (middle C)
        CHECK(tokens[0].type == TokenType::PitchLit);
        CHECK(tokens[0].as_pitch() == 60);

        // A4 = 69 (concert pitch)
        CHECK(tokens[1].type == TokenType::PitchLit);
        CHECK(tokens[1].as_pitch() == 69);

        // G3 = 55
        CHECK(tokens[2].type == TokenType::PitchLit);
        CHECK(tokens[2].as_pitch() == 55);
    }

    SECTION("sharps") {
        auto [tokens, diags] = lex("'c#4' 'f#3'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        // C#4 = 61
        CHECK(tokens[0].type == TokenType::PitchLit);
        CHECK(tokens[0].as_pitch() == 61);

        // F#3 = 54
        CHECK(tokens[1].type == TokenType::PitchLit);
        CHECK(tokens[1].as_pitch() == 54);
    }

    SECTION("flats") {
        auto [tokens, diags] = lex("'bb3' 'eb4'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        // Bb3 = 58 (B3=59, Bb3=58)
        CHECK(tokens[0].type == TokenType::PitchLit);
        CHECK(tokens[0].as_pitch() == 58);

        // Eb4 = 63 (E4=64, Eb4=63)
        CHECK(tokens[1].type == TokenType::PitchLit);
        CHECK(tokens[1].as_pitch() == 63);
    }

    SECTION("uppercase note names") {
        auto [tokens, diags] = lex("'C4' 'A#2' 'Bb5'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        CHECK(tokens[0].type == TokenType::PitchLit);
        CHECK(tokens[0].as_pitch() == 60);  // C4

        CHECK(tokens[1].type == TokenType::PitchLit);
        CHECK(tokens[1].as_pitch() == 46);  // A#2

        CHECK(tokens[2].type == TokenType::PitchLit);
        CHECK(tokens[2].as_pitch() == 82);  // Bb5
    }

    SECTION("extreme octaves") {
        auto [tokens, diags] = lex("'c0' 'c10'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        // C0 = 12
        CHECK(tokens[0].type == TokenType::PitchLit);
        CHECK(tokens[0].as_pitch() == 12);

        // C10 would be 132, but clamped to 127
        CHECK(tokens[1].type == TokenType::PitchLit);
        CHECK(tokens[1].as_pitch() == 127);
    }

    SECTION("non-pitch single-quoted strings remain strings") {
        // 'hello' should still be a string, not a pitch
        auto [tokens, diags] = lex("'hello'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::String);
        CHECK(tokens[0].as_string() == "hello");
    }
}

TEST_CASE("Lexer chord literals", "[lexer]") {
    SECTION("major chord") {
        auto [tokens, diags] = lex("'c4:maj'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::ChordLit);
        auto& chord = tokens[0].as_chord();
        CHECK(chord.root_midi == 60);  // C4
        REQUIRE(chord.intervals.size() == 3);
        CHECK(chord.intervals[0] == 0);  // root
        CHECK(chord.intervals[1] == 4);  // major third
        CHECK(chord.intervals[2] == 7);  // perfect fifth
    }

    SECTION("minor chord with sharp") {
        auto [tokens, diags] = lex("'f#3:min'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::ChordLit);
        auto& chord = tokens[0].as_chord();
        CHECK(chord.root_midi == 54);  // F#3
        REQUIRE(chord.intervals.size() == 3);
        CHECK(chord.intervals[0] == 0);   // root
        CHECK(chord.intervals[1] == 3);   // minor third
        CHECK(chord.intervals[2] == 7);   // perfect fifth
    }

    SECTION("seventh chord") {
        auto [tokens, diags] = lex("'a3:dom7'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::ChordLit);
        auto& chord = tokens[0].as_chord();
        CHECK(chord.root_midi == 57);  // A3
        REQUIRE(chord.intervals.size() == 4);
        CHECK(chord.intervals[0] == 0);   // root
        CHECK(chord.intervals[1] == 4);   // major third
        CHECK(chord.intervals[2] == 7);   // perfect fifth
        CHECK(chord.intervals[3] == 10);  // minor seventh
    }

    SECTION("power chord") {
        auto [tokens, diags] = lex("'e2:5'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::ChordLit);
        auto& chord = tokens[0].as_chord();
        CHECK(chord.root_midi == 40);  // E2
        REQUIRE(chord.intervals.size() == 2);
        CHECK(chord.intervals[0] == 0);  // root
        CHECK(chord.intervals[1] == 7);  // perfect fifth
    }

    SECTION("unknown chord type falls back to string") {
        // 'c4:xyz' should be a string since 'xyz' is not a known chord
        auto [tokens, diags] = lex("'c4:xyz'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::String);
        CHECK(tokens[0].as_string() == "c4:xyz");
    }
}

TEST_CASE("Lexer strings", "[lexer]") {
    SECTION("double quoted") {
        auto [tokens, diags] = lex(R"("hello world")");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::String);
        CHECK(tokens[0].as_string() == "hello world");
    }

    SECTION("single quoted") {
        auto [tokens, diags] = lex("'hello'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::String);
        CHECK(tokens[0].as_string() == "hello");
    }

    SECTION("backtick quoted") {
        auto [tokens, diags] = lex("`mini notation`");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::String);
        CHECK(tokens[0].as_string() == "mini notation");
    }

    SECTION("escape sequences") {
        auto [tokens, diags] = lex(R"("line1\nline2\ttab\\slash")");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].as_string() == "line1\nline2\ttab\\slash");
    }

    SECTION("multiline string") {
        auto [tokens, diags] = lex("'line1\nline2\nline3'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].as_string() == "line1\nline2\nline3");
    }

    SECTION("unterminated string error") {
        auto [tokens, diags] = lex("\"hello");
        CHECK(!diags.empty());
        CHECK(tokens[0].type == TokenType::Error);
    }
}

TEST_CASE("Lexer identifiers", "[lexer]") {
    SECTION("simple identifiers") {
        auto [tokens, diags] = lex("foo bar baz");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "foo");

        CHECK(tokens[1].type == TokenType::Identifier);
        CHECK(tokens[1].as_string() == "bar");

        CHECK(tokens[2].type == TokenType::Identifier);
        CHECK(tokens[2].as_string() == "baz");
    }

    SECTION("identifiers with underscores and numbers") {
        auto [tokens, diags] = lex("foo_bar baz123 _private x1y2z3");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 5);

        CHECK(tokens[0].as_string() == "foo_bar");
        CHECK(tokens[1].as_string() == "baz123");
        CHECK(tokens[2].as_string() == "_private");
        CHECK(tokens[3].as_string() == "x1y2z3");
    }

    SECTION("underscore alone is token") {
        auto [tokens, diags] = lex("_ foo");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == TokenType::Underscore);
        CHECK(tokens[1].type == TokenType::Identifier);
    }
}

TEST_CASE("Lexer keywords", "[lexer]") {
    SECTION("boolean keywords") {
        auto [tokens, diags] = lex("true false");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == TokenType::True);
        CHECK(tokens[1].type == TokenType::False);
    }

    SECTION("pattern keywords") {
        auto [tokens, diags] = lex("pat seq timeline note");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 5);

        CHECK(tokens[0].type == TokenType::Pat);
        CHECK(tokens[1].type == TokenType::Seq);
        CHECK(tokens[2].type == TokenType::Timeline);
        CHECK(tokens[3].type == TokenType::Note);
    }

    SECTION("post keyword") {
        auto [tokens, diags] = lex("post");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::Post);
    }

    SECTION("keywords are case sensitive") {
        auto [tokens, diags] = lex("True FALSE Post");
        REQUIRE(diags.empty());

        // These should be identifiers, not keywords
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::Identifier);
        CHECK(tokens[2].type == TokenType::Identifier);
    }
}

TEST_CASE("Lexer comments", "[lexer]") {
    SECTION("line comment") {
        auto [tokens, diags] = lex("foo // this is a comment\nbar");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "foo");

        CHECK(tokens[1].type == TokenType::Identifier);
        CHECK(tokens[1].as_string() == "bar");
    }

    SECTION("comment at end of file") {
        auto [tokens, diags] = lex("foo // comment");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::Identifier);
    }

    SECTION("comment only") {
        auto [tokens, diags] = lex("// just a comment");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 1);

        CHECK(tokens[0].type == TokenType::Eof);
    }
}

TEST_CASE("Lexer source locations", "[lexer]") {
    SECTION("single line positions") {
        auto [tokens, diags] = lex("foo bar");
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].location.line == 1);
        CHECK(tokens[0].location.column == 1);
        CHECK(tokens[0].location.offset == 0);
        CHECK(tokens[0].location.length == 3);

        CHECK(tokens[1].location.line == 1);
        CHECK(tokens[1].location.column == 5);
        CHECK(tokens[1].location.offset == 4);
        CHECK(tokens[1].location.length == 3);
    }

    SECTION("multi-line positions") {
        auto [tokens, diags] = lex("foo\nbar\nbaz");
        REQUIRE(tokens.size() == 4);

        CHECK(tokens[0].location.line == 1);
        CHECK(tokens[1].location.line == 2);
        CHECK(tokens[2].location.line == 3);
    }

    SECTION("lexeme matches source") {
        auto [tokens, diags] = lex("hello 42 |>");
        REQUIRE(tokens.size() == 4);

        CHECK(tokens[0].lexeme == "hello");
        CHECK(tokens[1].lexeme == "42");
        CHECK(tokens[2].lexeme == "|>");
    }
}

TEST_CASE("Lexer method calls", "[lexer]") {
    SECTION("simple method call") {
        auto [tokens, diags] = lex("x.foo()");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 6); // x . foo ( ) Eof

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].lexeme == "x");
        CHECK(tokens[1].type == TokenType::Dot);
        CHECK(tokens[2].type == TokenType::Identifier);
        CHECK(tokens[2].lexeme == "foo");
        CHECK(tokens[3].type == TokenType::LParen);
        CHECK(tokens[4].type == TokenType::RParen);
    }

    SECTION("method call with args") {
        auto [tokens, diags] = lex("osc.filter(1000, 0.5)");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 9); // osc . filter ( 1000 , 0.5 ) Eof

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::Dot);
        CHECK(tokens[2].type == TokenType::Identifier);
        CHECK(tokens[3].type == TokenType::LParen);
        CHECK(tokens[4].type == TokenType::Number);
        CHECK(tokens[5].type == TokenType::Comma);
        CHECK(tokens[6].type == TokenType::Number);
        CHECK(tokens[7].type == TokenType::RParen);
    }

    SECTION("chained method calls") {
        auto [tokens, diags] = lex("x.foo().bar()");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 10); // x . foo ( ) . bar ( ) Eof

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::Dot);
        CHECK(tokens[2].type == TokenType::Identifier);
        CHECK(tokens[3].type == TokenType::LParen);
        CHECK(tokens[4].type == TokenType::RParen);
        CHECK(tokens[5].type == TokenType::Dot);
        CHECK(tokens[6].type == TokenType::Identifier);
        CHECK(tokens[7].type == TokenType::LParen);
        CHECK(tokens[8].type == TokenType::RParen);
    }

    SECTION("dot with number (not method)") {
        auto [tokens, diags] = lex("3.14");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2); // 3.14 Eof (single number token)

        CHECK(tokens[0].type == TokenType::Number);
        CHECK_THAT(tokens[0].as_number(), WithinRel(3.14));
    }
}

TEST_CASE("Lexer complex expressions", "[lexer]") {
    SECTION("assignment") {
        auto [tokens, diags] = lex("bpm = 120");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::Equals);
        CHECK(tokens[2].type == TokenType::Number);
    }

    SECTION("pipe expression") {
        auto [tokens, diags] = lex("saw(440) |> lp(%, 1000)");
        REQUIRE(diags.empty());

        // saw ( 440 ) |> lp ( % , 1000 )
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::LParen);
        CHECK(tokens[2].type == TokenType::Number);
        CHECK(tokens[3].type == TokenType::RParen);
        CHECK(tokens[4].type == TokenType::Pipe);
        CHECK(tokens[5].type == TokenType::Identifier);
        CHECK(tokens[6].type == TokenType::LParen);
        CHECK(tokens[7].type == TokenType::Hole);
        CHECK(tokens[8].type == TokenType::Comma);
        CHECK(tokens[9].type == TokenType::Number);
        CHECK(tokens[10].type == TokenType::RParen);
    }

    SECTION("closure") {
        auto [tokens, diags] = lex("(x, y) -> x + y");
        REQUIRE(diags.empty());

        CHECK(tokens[0].type == TokenType::LParen);
        CHECK(tokens[1].type == TokenType::Identifier);
        CHECK(tokens[2].type == TokenType::Comma);
        CHECK(tokens[3].type == TokenType::Identifier);
        CHECK(tokens[4].type == TokenType::RParen);
        CHECK(tokens[5].type == TokenType::Arrow);
        CHECK(tokens[6].type == TokenType::Identifier);
        CHECK(tokens[7].type == TokenType::Plus);
        CHECK(tokens[8].type == TokenType::Identifier);
    }

    SECTION("pattern with closure") {
        auto [tokens, diags] = lex("seq('c4 e4 g4', (t, v, p) -> saw(p))");
        REQUIRE(diags.empty());

        CHECK(tokens[0].type == TokenType::Seq);
        CHECK(tokens[1].type == TokenType::LParen);
        CHECK(tokens[2].type == TokenType::String);
        CHECK(tokens[2].as_string() == "c4 e4 g4");
    }

    SECTION("math expression") {
        auto [tokens, diags] = lex("400 + 300 * sin(hz: 1/16 * co)");
        REQUIRE(diags.empty());

        CHECK(tokens[0].type == TokenType::Number);
        CHECK(tokens[1].type == TokenType::Plus);
        CHECK(tokens[2].type == TokenType::Number);
        CHECK(tokens[3].type == TokenType::Star);
        CHECK(tokens[4].type == TokenType::Identifier); // sin
        CHECK(tokens[5].type == TokenType::LParen);
        CHECK(tokens[6].type == TokenType::Identifier); // hz
        CHECK(tokens[7].type == TokenType::Colon);
    }
}

TEST_CASE("Lexer error recovery", "[lexer]") {
    SECTION("continues after error") {
        auto [tokens, diags] = lex("foo | bar"); // | alone is error
        CHECK(!diags.empty());

        // Should still find 'bar' after the error
        bool found_bar = false;
        for (const auto& tok : tokens) {
            if (tok.type == TokenType::Identifier && tok.lexeme == "bar") {
                found_bar = true;
                break;
            }
        }
        CHECK(found_bar);
    }
}
