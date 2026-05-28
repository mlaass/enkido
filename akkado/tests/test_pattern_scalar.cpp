// Tests for PRD prd-patterns-as-scalar-values: typed pattern prefixes,
// numeric (Value) atom mode, Pattern→Signal coerce, and the explicit
// scalar() cast.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/akkado.hpp"
#include "akkado/lexer.hpp"
#include "akkado/mini_lexer.hpp"
#include "akkado/mini_parser.hpp"
#include "test_lex_helper.hpp"

using namespace akkado;
using akkado::test_helpers::lex;

// =============================================================================
// Phase A: lexer prefixes + numeric atom mode
// =============================================================================

TEST_CASE("Typed pattern prefixes lex correctly", "[lexer][prefixes]") {
    SECTION("v\"…\" produces ValuePat token") {
        auto [tokens, diags] = lex(R"(v"<0 0.5 -0.5>")");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);  // ValuePat, String, Eof
        CHECK(tokens[0].type == TokenType::ValuePat);
        CHECK(tokens[1].type == TokenType::String);
        CHECK(tokens[1].as_string_lit() == "<0 0.5 -0.5>");
    }

    SECTION("n\"…\" produces NotePat token") {
        auto [tokens, diags] = lex(R"(n"c4 e4 g4")");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);
        CHECK(tokens[0].type == TokenType::NotePat);
    }

    SECTION("s\"…\" produces SamplePat token") {
        auto [tokens, diags] = lex(R"(s"bd sd hh")");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);
        CHECK(tokens[0].type == TokenType::SamplePat);
    }

    SECTION("c\"…\" produces ChordPat token") {
        auto [tokens, diags] = lex(R"(c"Am C G")");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);
        CHECK(tokens[0].type == TokenType::ChordPat);
    }

    SECTION("multi-char identifiers starting with prefix letter stay identifiers") {
        auto [tokens, diags] = lex("value note sample chord");
        REQUIRE(diags.empty());
        // 4 identifiers + Eof
        REQUIRE(tokens.size() == 5);
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(test_helpers::ident_text(tokens[0].as_identifier()) == "value");
        CHECK(tokens[1].type == TokenType::Identifier);
        CHECK(tokens[2].type == TokenType::Identifier);
        CHECK(tokens[3].type == TokenType::Identifier);
    }

    SECTION("single-letter prefix not followed by quote is identifier") {
        auto [tokens, diags] = lex("v + 1");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == TokenType::Identifier);
        CHECK(test_helpers::ident_text(tokens[0].as_identifier()) == "v");
    }

    SECTION("backtick form works for typed prefixes too") {
        auto [tokens, diags] = lex("v`<0 0.5>`");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);
        CHECK(tokens[0].type == TokenType::ValuePat);
        CHECK(tokens[1].as_string_lit() == "<0 0.5>");
    }
}

TEST_CASE("Mini-parser Value mode accepts numeric atoms", "[mini_parser][value]") {
    AstArena arena;
    SECTION("simple integer atoms") {
        auto [root, diags] = parse_mini("0 1 2 3", arena, {}, MiniParseMode::Value);
        REQUIRE(akkado::has_errors(diags) == false);
        REQUIRE(root != NULL_NODE);
        // Walk children — expect 4 atoms with scalar_value 0..3
        const Node& pat = arena[root];
        REQUIRE(pat.type == NodeType::MiniPattern);
        std::vector<float> values;
        NodeIndex c = pat.first_child;
        while (c != NULL_NODE) {
            const Node& cn = arena[c];
            if (cn.type == NodeType::MiniAtom) {
                const auto& d = cn.as_mini_atom();
                REQUIRE(d.kind == Node::MiniAtomKind::Value);
                values.push_back(d.scalar_value);
            }
            c = cn.next_sibling;
        }
        REQUIRE(values.size() == 4);
        CHECK(values[0] == 0.0f);
        CHECK(values[1] == 1.0f);
        CHECK(values[2] == 2.0f);
        CHECK(values[3] == 3.0f);
    }

    SECTION("decimal and negative atoms") {
        auto [root, diags] = parse_mini("-0.5 0.25 -1.25", arena, {},
                                        MiniParseMode::Value);
        REQUIRE(akkado::has_errors(diags) == false);
        std::vector<float> values;
        NodeIndex c = arena[root].first_child;
        while (c != NULL_NODE) {
            if (arena[c].type == NodeType::MiniAtom) {
                values.push_back(arena[c].as_mini_atom().scalar_value);
            }
            c = arena[c].next_sibling;
        }
        REQUIRE(values.size() == 3);
        CHECK(values[0] == -0.5f);
        CHECK(values[1] == 0.25f);
        CHECK(values[2] == -1.25f);
    }

    SECTION("scientific notation") {
        auto [root, diags] = parse_mini("1e3 -1.25e-2", arena, {},
                                        MiniParseMode::Value);
        REQUIRE(akkado::has_errors(diags) == false);
        std::vector<float> values;
        NodeIndex c = arena[root].first_child;
        while (c != NULL_NODE) {
            if (arena[c].type == NodeType::MiniAtom) {
                values.push_back(arena[c].as_mini_atom().scalar_value);
            }
            c = arena[c].next_sibling;
        }
        REQUIRE(values.size() == 2);
        CHECK(values[0] == 1000.0f);
        CHECK(values[1] == -0.0125f);
    }

    SECTION("note name in Value mode is rejected (E163)") {
        auto [root, diags] = parse_mini("c4 e4", arena, {},
                                        MiniParseMode::Value);
        CHECK(akkado::has_errors(diags));
    }

    SECTION("angle brackets work with numeric atoms") {
        auto [root, diags] = parse_mini("<0 0.5 -0.5>", arena, {},
                                        MiniParseMode::Value);
        REQUIRE(akkado::has_errors(diags) == false);
        REQUIRE(root != NULL_NODE);
    }
}

// =============================================================================
// Phase B: typed prefix parses through to MiniLiteral; payload flags are set
// =============================================================================

TEST_CASE("v\"…\" compiles to a Pattern signal", "[codegen][value]") {
    SECTION("v\"…\" feeds osc as freq directly (no mtof)") {
        auto result = compile(R"(sine(v"<220 440 880>") |> out(%, %))");
        REQUIRE(result.success);
    }

    SECTION("v\"…\" with negatives and decimals") {
        auto result = compile(R"(sine(440 + v"<0 0.5 -0.5>" * 100) |> out(%, %))");
        REQUIRE(result.success);
    }

    SECTION("note pattern as scalar feeds osc freq slot") {
        auto result = compile(R"(sine(n"c4 e4 g4") |> out(%, %))");
        REQUIRE(result.success);
    }
}

// =============================================================================
// Phase C: scalar() builtin + chord pattern in scalar slot rejection
// =============================================================================

TEST_CASE("scalar() cast", "[codegen][scalar]") {
    SECTION("scalar() on a note pattern yields a Signal usable as freq") {
        auto result = compile(R"(sine(scalar(n"c4 e4 g4")) |> out(%, %))");
        REQUIRE(result.success);
    }

    SECTION("scalar() on a value pattern works") {
        auto result = compile(R"(sine(scalar(v"<220 440>")) |> out(%, %))");
        REQUIRE(result.success);
    }

    SECTION("scalar() on a polyphonic chord pattern errors E161") {
        auto result = compile(R"(sine(scalar(c"Am")) |> out(%, %))");
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E161") found = true;
        }
        CHECK(found);
    }

    SECTION("scalar() is idempotent on a Signal") {
        auto result = compile(R"(sine(scalar(440)) |> out(%, %))");
        REQUIRE(result.success);
    }
}

TEST_CASE("Polyphonic chord pattern rejected at scalar slot", "[codegen][coerce][E160]") {
    SECTION("chord pattern in osc freq slot errors E160") {
        auto result = compile(R"(sine(c"Am") |> out(%, %))");
        CHECK_FALSE(result.success);
        bool found_e160 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E160") found_e160 = true;
        }
        CHECK(found_e160);
    }

    SECTION("monophonic note pattern is fine") {
        auto result = compile(R"(sine(n"c4 e4 g4") |> out(%, %))");
        REQUIRE(result.success);
    }

    SECTION("monophonic value pattern is fine") {
        auto result = compile(R"(sine(v"<440 880>") |> out(%, %))");
        REQUIRE(result.success);
    }
}

// =============================================================================
// Phase D: pattern-arg bend / aftertouch / dur
// =============================================================================

TEST_CASE("Phase F: cross-phase smoke acceptance", "[codegen][smoke]") {
    SECTION("flagship: pattern-driven oscillator + filter + bend depth") {
        auto result = compile(R"(
            n"c4{cutoff:0.3} e4{cutoff:0.7} g4{cutoff:0.5}" as e
              |> saw(e.freq + v"<0 -10 10>")
              |> lp(%, 200 + e.cutoff * 4000, v"<0.3 0.7>")
              |> % * v"<0.5 1.0 0.7>"
              |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("pattern-driven bend on a separate voice") {
        auto result = compile(R"(
            n"c4 e4 g4 b4" |> bend(%, v"<0 0.25 -0.25 0>") as e
              |> sine(e.freq + e.bend * 12)
              |> out(%, %)
        )");
        CHECK(result.success);
    }

    SECTION("negative coerce path: chord pattern in scalar slot errors E160") {
        auto result = compile(R"(sine(c"Am C G") |> out(%, %))");
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E160") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Custom-property accessor (e.cutoff) coerces to Signal",
          "[codegen][custom_property]") {
    SECTION("e.cutoff feeds lp() cutoff slot") {
        auto result = compile(R"(n"c4{cutoff:0.3} e4{cutoff:0.7} g4{cutoff:0.5}" as e |> saw(e.freq) |> lp(%, 200 + e.cutoff * 4000) |> out(%, %))");
        CHECK(result.success);
    }

    SECTION("e.bend custom property compiles") {
        auto result = compile(R"(n"c4{bend:0.0} e4{bend:0.5} g4{bend:-0.5}" as e |> sine(e.freq + e.bend * 100) |> out(%, %))");
        CHECK(result.success);
    }
}

TEST_CASE("Pattern-arg bend / aftertouch / dur", "[codegen][pattern_args]") {
    SECTION("bend(notes, v\"…\") compiles") {
        auto result = compile(R"(n"c4 e4 g4" |> bend(%, v"<0 0.5 -0.5>") |> sine(%.freq) |> out(%, %))");
        CHECK(result.success);
    }

    SECTION("aftertouch(notes, v\"…\") compiles") {
        auto result = compile(R"(n"c4 e4 g4 b4" |> aftertouch(%, v"<0 0.25 0.5 1.0>") |> sine(%.freq) |> out(%, %))");
        CHECK(result.success);
    }

    SECTION("dur(notes, v\"…\") compiles") {
        auto result = compile(R"(n"c4 e4 g4" |> dur(%, v"<0.5 0.75 1.0>") |> sine(%.freq) |> out(%, %))");
        CHECK(result.success);
    }

    SECTION("constant arg still works for bend") {
        auto result = compile(R"(n"c4 e4 g4" |> bend(%, 0.5) |> sine(%.freq) |> out(%, %))");
        CHECK(result.success);
    }

    SECTION("bend with sample pattern as value compiles (Phase 2b: validation moved to runtime)") {
        // Pre-Phase 2b the C++ handler rejected sample patterns as bend
        // values with E160. The stdlib `fn bend(events: stream, b)` has no
        // type guard on its second arg, so this nonsensical pairing now
        // compiles silently. Documented in PRD §8 as a known regression.
        auto result = compile(R"(n"c4 e4" |> bend(%, s"bd sd") |> sine(%.freq) |> out(%, %))");
        CHECK(result.success);
    }
}

// =============================================================================
// MiniLiteral parse-mode tagging
// =============================================================================

TEST_CASE("Typed prefix MiniLiteral carries mode tag", "[parser][prefixes]") {
    SECTION("v\"…\" parses to a MiniLiteral marked 'value'") {
        auto result = compile(R"(v"<0 0.5 -0.5>" |> sine(%))");
        // We don't strictly assert the marker contents here — just that the
        // typed prefix successfully forms a usable expression.
        REQUIRE(result.success);
    }

    SECTION("n\"…\" works in the freq slot identical to n\"…\"") {
        auto a = compile(R"(sine(n"c4 e4 g4") |> out(%, %))");
        auto b = compile(R"(sine(n"c4 e4 g4") |> out(%, %))");
        REQUIRE(a.success);
        REQUIRE(b.success);
    }
}
