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

    SECTION("diamond operator") {
        // `<>` is one token via maximal munch.
        auto [tokens, diags] = lex("<>");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);  // Diamond + Eof
        CHECK(tokens[0].type == TokenType::Diamond);
        CHECK(tokens[0].lexeme == "<>");
    }

    SECTION("diamond does not disturb `<` / `<=`") {
        // A spaced `< 3` stays a comparison; `<=` is unaffected.
        auto [tokens, diags] = lex("gain < 3 <= 5");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::Less);
        CHECK(tokens[2].type == TokenType::Number);
        CHECK(tokens[3].type == TokenType::LessEqual);
        CHECK(tokens[4].type == TokenType::Number);
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

TEST_CASE("Lexer no longer recognizes ChordLit syntax", "[lexer]") {
    // The Strudel-style chord literal (`C4'`) was removed; chord data is now
    // surfaced through pattern events via notes()/freqs(). `C4'` lexes as an
    // ordinary identifier `C4` followed by a quote — never a chord token.
    SECTION("uppercase chord-like token is just an identifier") {
        auto [tokens, diags] = lex("C4");
        REQUIRE(tokens.size() == 2);
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "C4");
    }

    SECTION("pitch literal format still works") {
        auto [tokens, diags] = lex("'c4'");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::PitchLit);
        CHECK(tokens[0].as_pitch() == 60);  // C4
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

    SECTION("pat is an identifier (post-removal)") {
        auto [tokens, diags] = lex("pat");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "pat");
    }

    SECTION("p\"...\" splits as identifier + string (post-removal)") {
        auto [tokens, diags] = lex(R"(p"c4 e4")");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3); // Identifier, String, Eof

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "p");
        CHECK(tokens[1].type == TokenType::String);
        CHECK(tokens[1].as_string() == "c4 e4");
    }

    SECTION("p`...` splits as identifier + string (post-removal)") {
        auto [tokens, diags] = lex("p`c4 e4`");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3); // Identifier, String, Eof

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "p");
        CHECK(tokens[1].type == TokenType::String);
        CHECK(tokens[1].as_string() == "c4 e4");
    }

    SECTION("p followed by non-quote is identifier") {
        auto [tokens, diags] = lex("p + 1");
        REQUIRE(diags.empty());

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "p");
    }

    SECTION("keywords are case sensitive") {
        auto [tokens, diags] = lex("True FALSE Post");
        REQUIRE(diags.empty());

        // These should be identifiers, not keywords
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::Identifier);
        CHECK(tokens[2].type == TokenType::Identifier);
    }

    // PRD prd-parameter-type-annotations + Phase 2 §6: `evs`, `sig`, `signal`,
    // `num`, `rec`, `arr`, `str` are reserved keywords used in `fn` parameter
    // annotations. `fn` was already reserved as the declaration keyword.
    SECTION("evs and sig are reserved keywords") {
        auto [tokens, diags] = lex("evs sig");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == TokenType::Evs);
        CHECK(tokens[1].type == TokenType::Signal);
    }

    SECTION("signal still tokenises (long-form alias of sig)") {
        auto [tokens, diags] = lex("signal");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::Signal);
    }

    SECTION("rec and arr lex to Record/Array tokens") {
        auto [tokens, diags] = lex("rec arr");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == TokenType::Record);
        CHECK(tokens[1].type == TokenType::Array);
    }

    SECTION("num keyword lexes to Number-typed token with empty value variant") {
        auto [tokens, diags] = lex("num");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::Number);
        CHECK(tokens[0].lexeme == "num");
        // The lexer keyword path does not populate the value variant — the
        // parser disambiguates `num` (keyword) vs literal `42` (NumericValue)
        // by inspecting tok.lexeme. Verify the variant is monostate.
        CHECK(std::holds_alternative<std::monostate>(tokens[0].value));
    }

    SECTION("str keyword lexes to String-typed token with empty value variant") {
        auto [tokens, diags] = lex("str");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        CHECK(tokens[0].type == TokenType::String);
        CHECK(tokens[0].lexeme == "str");
        CHECK(std::holds_alternative<std::monostate>(tokens[0].value));
    }

    SECTION("stream no longer lexes as a keyword (PRD Phase 2 rename)") {
        auto [tokens, diags] = lex("stream");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        // After the Phase 2 rename, `stream` is no longer in the keyword
        // table — it lexes as a plain identifier (and would surface as
        // E185 / parse error if used in annotation position).
        CHECK(tokens[0].type == TokenType::Identifier);
    }

    SECTION("evs/sig case sensitivity") {
        auto [tokens, diags] = lex("Evs SIG StReAm");
        REQUIRE(diags.empty());

        // Case-mismatched forms remain identifiers.
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

    SECTION(">> alias for pipe") {
        auto [tokens, diags] = lex("saw(440) >> lp(@, 1000)");
        REQUIRE(diags.empty());

        // saw ( 440 ) >> lp ( @ , 1000 )
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::LParen);
        CHECK(tokens[2].type == TokenType::Number);
        CHECK(tokens[3].type == TokenType::RParen);
        CHECK(tokens[4].type == TokenType::Pipe);
        CHECK(tokens[5].type == TokenType::Identifier);
        CHECK(tokens[6].type == TokenType::LParen);
        CHECK(tokens[7].type == TokenType::At);
        CHECK(tokens[8].type == TokenType::Comma);
        CHECK(tokens[9].type == TokenType::Number);
        CHECK(tokens[10].type == TokenType::RParen);
    }

    SECTION(">> does not break >= or >") {
        auto [tokens, diags] = lex("a > b >= c");
        REQUIRE(diags.empty());

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[1].type == TokenType::Greater);
        CHECK(tokens[2].type == TokenType::Identifier);
        CHECK(tokens[3].type == TokenType::GreaterEqual);
        CHECK(tokens[4].type == TokenType::Identifier);
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

    SECTION("pat-as-identifier in call form") {
        // pat() is no longer a builtin; lexes as plain identifier + parens.
        auto [tokens, diags] = lex("pat('c4 e4 g4')");
        REQUIRE(diags.empty());

        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "pat");
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

// ============================================================================
// Token Type Name Tests [lexer]
// ============================================================================

TEST_CASE("token_type_name returns correct strings", "[lexer]") {
    CHECK(token_type_name(TokenType::Eof) == "Eof");
    CHECK(token_type_name(TokenType::Number) == "Number");
    CHECK(token_type_name(TokenType::String) == "String");
    CHECK(token_type_name(TokenType::Identifier) == "Identifier");
    CHECK(token_type_name(TokenType::PitchLit) == "PitchLit");
    CHECK(token_type_name(TokenType::True) == "True");
    CHECK(token_type_name(TokenType::False) == "False");
    CHECK(token_type_name(TokenType::Match) == "Match");
    CHECK(token_type_name(TokenType::Fn) == "Fn");
    CHECK(token_type_name(TokenType::As) == "As");
    CHECK(token_type_name(TokenType::Plus) == "Plus");
    CHECK(token_type_name(TokenType::Minus) == "Minus");
    CHECK(token_type_name(TokenType::Star) == "Star");
    CHECK(token_type_name(TokenType::Slash) == "Slash");
    CHECK(token_type_name(TokenType::Caret) == "Caret");
    CHECK(token_type_name(TokenType::Dot) == "Dot");
    CHECK(token_type_name(TokenType::Pipe) == "Pipe");
    CHECK(token_type_name(TokenType::Equals) == "Equals");
    CHECK(token_type_name(TokenType::Arrow) == "Arrow");
    CHECK(token_type_name(TokenType::Less) == "Less");
    CHECK(token_type_name(TokenType::Greater) == "Greater");
    CHECK(token_type_name(TokenType::LessEqual) == "LessEqual");
    CHECK(token_type_name(TokenType::GreaterEqual) == "GreaterEqual");
    CHECK(token_type_name(TokenType::EqualEqual) == "EqualEqual");
    CHECK(token_type_name(TokenType::BangEqual) == "BangEqual");
    CHECK(token_type_name(TokenType::AndAnd) == "AndAnd");
    CHECK(token_type_name(TokenType::OrOr) == "OrOr");
    CHECK(token_type_name(TokenType::LParen) == "LParen");
    CHECK(token_type_name(TokenType::RParen) == "RParen");
    CHECK(token_type_name(TokenType::LBracket) == "LBracket");
    CHECK(token_type_name(TokenType::RBracket) == "RBracket");
    CHECK(token_type_name(TokenType::LBrace) == "LBrace");
    CHECK(token_type_name(TokenType::RBrace) == "RBrace");
    CHECK(token_type_name(TokenType::Comma) == "Comma");
    CHECK(token_type_name(TokenType::Colon) == "Colon");
    CHECK(token_type_name(TokenType::Semicolon) == "Semicolon");
    CHECK(token_type_name(TokenType::Hole) == "Hole");
    CHECK(token_type_name(TokenType::At) == "At");
    CHECK(token_type_name(TokenType::Bang) == "Bang");
    CHECK(token_type_name(TokenType::Question) == "Question");
    CHECK(token_type_name(TokenType::Tilde) == "Tilde");
    CHECK(token_type_name(TokenType::Underscore) == "Underscore");
    CHECK(token_type_name(TokenType::MiniString) == "MiniString");
    CHECK(token_type_name(TokenType::Error) == "Error");
}

// ============================================================================
// Token Helper Method Tests [lexer]
// ============================================================================

TEST_CASE("Token is_error and is_eof helpers", "[lexer]") {
    SECTION("is_eof on EOF token") {
        auto [tokens, diags] = lex("");
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].is_eof());
        CHECK_FALSE(tokens[0].is_error());
    }

    SECTION("is_error on error token") {
        auto [tokens, diags] = lex("\"unterminated");
        // Find the error token
        bool found_error = false;
        for (const auto& tok : tokens) {
            if (tok.is_error()) {
                found_error = true;
                CHECK_FALSE(tok.is_eof());
                break;
            }
        }
        CHECK(found_error);
    }

    SECTION("neither eof nor error on normal token") {
        auto [tokens, diags] = lex("42");
        REQUIRE(tokens.size() == 2);
        CHECK_FALSE(tokens[0].is_eof());
        CHECK_FALSE(tokens[0].is_error());
    }
}

// ============================================================================
// Token Accessor Edge Cases [lexer]
// ============================================================================

TEST_CASE("Token accessors edge cases", "[lexer]") {
    SECTION("as_pitch for extreme octaves") {
        // Lowest octave
        auto [tokens1, diags1] = lex("'c0'");
        REQUIRE(diags1.empty());
        CHECK(tokens1[0].as_pitch() == 12);  // C0

        // Highest clamped
        auto [tokens2, diags2] = lex("'c10'");
        REQUIRE(diags2.empty());
        CHECK(tokens2[0].as_pitch() == 127);
    }

    SECTION("as_number for scientific notation") {
        auto [tokens, diags] = lex("1.5e-3");
        REQUIRE(diags.empty());
        CHECK_THAT(tokens[0].as_number(), WithinRel(0.0015));
    }

    SECTION("as_string for escaped characters") {
        auto [tokens, diags] = lex(R"("tab\there")");
        REQUIRE(diags.empty());
        CHECK(tokens[0].as_string() == "tab\there");
    }
}

TEST_CASE("Lexer timeline string prefix", "[lexer][timeline]") {
    SECTION("t followed by double quote is Timeline token") {
        auto [tokens, diags] = lex("t\"__/''\"");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() >= 3);
        CHECK(tokens[0].type == TokenType::Timeline);
        CHECK(tokens[1].type == TokenType::String);
        CHECK(tokens[1].as_string() == "__/''");
    }

    SECTION("t followed by backtick is Timeline token") {
        auto [tokens, diags] = lex("t`__/''`");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Timeline);
        CHECK(tokens[1].type == TokenType::String);
    }

    SECTION("standalone t is Identifier") {
        auto [tokens, diags] = lex("t = 5");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Identifier);
    }

    SECTION("total is Identifier not Timeline") {
        auto [tokens, diags] = lex("total = 10");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(tokens[0].as_string() == "total");
    }
}
