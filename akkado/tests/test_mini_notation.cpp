#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/lexer.hpp"
#include "akkado/mini_lexer.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/tuning.hpp"

using namespace akkado;
using Catch::Matchers::WithinRel;

// ============================================================================
// Mini Token Type Name Tests [mini_lexer]
// ============================================================================

TEST_CASE("mini_token_type_name returns correct strings", "[mini_lexer]") {
    CHECK(mini_token_type_name(MiniTokenType::Eof) == "Eof");
    CHECK(mini_token_type_name(MiniTokenType::PitchToken) == "PitchToken");
    CHECK(mini_token_type_name(MiniTokenType::SampleToken) == "SampleToken");
    CHECK(mini_token_type_name(MiniTokenType::ChordToken) == "ChordToken");
    CHECK(mini_token_type_name(MiniTokenType::Rest) == "Rest");
    CHECK(mini_token_type_name(MiniTokenType::Number) == "Number");
    CHECK(mini_token_type_name(MiniTokenType::LBracket) == "LBracket");
    CHECK(mini_token_type_name(MiniTokenType::RBracket) == "RBracket");
    CHECK(mini_token_type_name(MiniTokenType::LAngle) == "LAngle");
    CHECK(mini_token_type_name(MiniTokenType::RAngle) == "RAngle");
    CHECK(mini_token_type_name(MiniTokenType::LParen) == "LParen");
    CHECK(mini_token_type_name(MiniTokenType::RParen) == "RParen");
    CHECK(mini_token_type_name(MiniTokenType::LBrace) == "LBrace");
    CHECK(mini_token_type_name(MiniTokenType::RBrace) == "RBrace");
    CHECK(mini_token_type_name(MiniTokenType::Comma) == "Comma");
    CHECK(mini_token_type_name(MiniTokenType::Star) == "Star");
    CHECK(mini_token_type_name(MiniTokenType::Slash) == "Slash");
    CHECK(mini_token_type_name(MiniTokenType::Colon) == "Colon");
    CHECK(mini_token_type_name(MiniTokenType::At) == "At");
    CHECK(mini_token_type_name(MiniTokenType::Bang) == "Bang");
    CHECK(mini_token_type_name(MiniTokenType::Question) == "Question");
    CHECK(mini_token_type_name(MiniTokenType::Percent) == "Percent");
    CHECK(mini_token_type_name(MiniTokenType::Pipe) == "Pipe");
    CHECK(mini_token_type_name(MiniTokenType::Error) == "Error");
}

// ============================================================================
// MiniToken Accessor Tests [mini_lexer]
// ============================================================================

TEST_CASE("MiniToken is_eof and is_error helpers", "[mini_lexer]") {
    SECTION("is_eof on EOF token") {
        auto [tokens, diags] = lex_mini("");
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].is_eof());
        CHECK_FALSE(tokens[0].is_error());
    }

    SECTION("neither eof nor error on normal token") {
        auto [tokens, diags] = lex_mini("c4");
        REQUIRE(tokens.size() >= 2);
        CHECK_FALSE(tokens[0].is_eof());
        CHECK_FALSE(tokens[0].is_error());
    }
}

TEST_CASE("MiniToken as_number accessor", "[mini_lexer]") {
    auto [tokens, diags] = lex_mini("c*2.5");
    REQUIRE(diags.empty());

    // Find the number token
    for (const auto& t : tokens) {
        if (t.type == MiniTokenType::Number) {
            CHECK_THAT(t.as_number(), WithinRel(2.5, 0.001));
            break;
        }
    }
}

TEST_CASE("MiniToken as_pitch accessor", "[mini_lexer]") {
    SECTION("pitch with explicit octave") {
        auto [tokens, diags] = lex_mini("c4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);

        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.has_octave == true);
    }

    SECTION("pitch without octave defaults to 4") {
        auto [tokens, diags] = lex_mini("e");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);

        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 64);  // E4
        CHECK(pitch.has_octave == false);
    }
}

TEST_CASE("MiniToken as_sample accessor", "[mini_lexer]") {
    SECTION("sample without variant") {
        auto [tokens, diags] = lex_mini("bd");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::SampleToken);

        const auto& sample = tokens[0].as_sample();
        CHECK(sample.name == "bd");
        CHECK(sample.variant == 0);
    }

    SECTION("sample with variant") {
        auto [tokens, diags] = lex_mini("kick:3");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::SampleToken);

        const auto& sample = tokens[0].as_sample();
        CHECK(sample.name == "kick");
        CHECK(sample.variant == 3);
    }
}

TEST_CASE("MiniToken as_chord accessor", "[mini_lexer]") {
    auto [tokens, diags] = lex_mini("Am7");
    REQUIRE(diags.empty());
    REQUIRE(tokens[0].type == MiniTokenType::ChordToken);

    const auto& chord = tokens[0].as_chord();
    CHECK(chord.root == "A");
    CHECK(chord.quality == "m7");
    CHECK(chord.root_midi == 69);  // A4
    REQUIRE(chord.intervals.size() == 4);
    CHECK(chord.intervals[0] == 0);   // root
    CHECK(chord.intervals[1] == 3);   // minor third
    CHECK(chord.intervals[2] == 7);   // fifth
    CHECK(chord.intervals[3] == 10);  // minor seventh
}

// ============================================================================
// Mini-Notation Lexer Tests
// ============================================================================

TEST_CASE("Mini lexer basic tokens", "[mini_lexer]") {
    SECTION("empty pattern") {
        auto [tokens, diags] = lex_mini("");
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].type == MiniTokenType::Eof);
        CHECK(diags.empty());
    }

    SECTION("whitespace only") {
        auto [tokens, diags] = lex_mini("   \t  ");
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].type == MiniTokenType::Eof);
    }

    SECTION("single pitch") {
        auto [tokens, diags] = lex_mini("c4");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2); // pitch + eof
        CHECK(tokens[0].type == MiniTokenType::PitchToken);
        CHECK(tokens[0].as_pitch().midi_note == 60); // C4 = 60
    }

    SECTION("pitch with accidentals") {
        auto [tokens, diags] = lex_mini("f#3 Bb5");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);
        CHECK(tokens[0].type == MiniTokenType::PitchToken);
        CHECK(tokens[0].as_pitch().midi_note == 54); // F#3
        CHECK(tokens[1].type == MiniTokenType::PitchToken);
        CHECK(tokens[1].as_pitch().midi_note == 82); // Bb5
    }

    SECTION("pitch without octave defaults to 4") {
        auto [tokens, diags] = lex_mini("c e g");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);
        CHECK(tokens[0].as_pitch().midi_note == 60); // C4
        CHECK(tokens[1].as_pitch().midi_note == 64); // E4
        CHECK(tokens[2].as_pitch().midi_note == 67); // G4
    }

    SECTION("sample tokens") {
        auto [tokens, diags] = lex_mini("bd sd hh");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);
        CHECK(tokens[0].type == MiniTokenType::SampleToken);
        CHECK(tokens[0].as_sample().name == "bd");
        CHECK(tokens[1].as_sample().name == "sd");
        CHECK(tokens[2].as_sample().name == "hh");
    }

    SECTION("sample with variant") {
        auto [tokens, diags] = lex_mini("bd:2 sd:1");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);
        CHECK(tokens[0].as_sample().name == "bd");
        CHECK(tokens[0].as_sample().variant == 2);
        CHECK(tokens[1].as_sample().name == "sd");
        CHECK(tokens[1].as_sample().variant == 1);
    }

    SECTION("rest and elongate tokens") {
        auto [tokens, diags] = lex_mini("~ _ ~");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);
        CHECK(tokens[0].type == MiniTokenType::Rest);
        CHECK(tokens[1].type == MiniTokenType::Elongate);  // _ is Elongate, not Rest
        CHECK(tokens[2].type == MiniTokenType::Rest);
    }

    SECTION("grouping tokens") {
        auto [tokens, diags] = lex_mini("[a b] <c d>");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == MiniTokenType::LBracket);
        CHECK(tokens[3].type == MiniTokenType::RBracket);
        CHECK(tokens[4].type == MiniTokenType::LAngle);
        CHECK(tokens[7].type == MiniTokenType::RAngle);
    }

    SECTION("modifier tokens") {
        auto [tokens, diags] = lex_mini("c*2 d/4 e!3 f?0.5 g@0.8");
        REQUIRE(diags.empty());
        // Check operator tokens appear
        bool found_star = false, found_slash = false, found_bang = false;
        bool found_question = false, found_at = false;
        for (const auto& t : tokens) {
            if (t.type == MiniTokenType::Star) found_star = true;
            if (t.type == MiniTokenType::Slash) found_slash = true;
            if (t.type == MiniTokenType::Bang) found_bang = true;
            if (t.type == MiniTokenType::Question) found_question = true;
            if (t.type == MiniTokenType::At) found_at = true;
        }
        CHECK(found_star);
        CHECK(found_slash);
        CHECK(found_bang);
        CHECK(found_question);
        CHECK(found_at);
    }

    SECTION("numbers") {
        auto [tokens, diags] = lex_mini("c*2.5");
        REQUIRE(diags.empty());
        bool found_number = false;
        for (const auto& t : tokens) {
            if (t.type == MiniTokenType::Number) {
                found_number = true;
                CHECK_THAT(t.as_number(), WithinRel(2.5, 0.001));
            }
        }
        CHECK(found_number);
    }

    SECTION("polymeter tokens") {
        auto [tokens, diags] = lex_mini("{bd sd}%5");
        REQUIRE(diags.empty());
        CHECK(tokens[0].type == MiniTokenType::LBrace);
        CHECK(tokens[1].type == MiniTokenType::SampleToken);
        CHECK(tokens[2].type == MiniTokenType::SampleToken);
        CHECK(tokens[3].type == MiniTokenType::RBrace);
        CHECK(tokens[4].type == MiniTokenType::Percent);
        CHECK(tokens[5].type == MiniTokenType::Number);
        CHECK_THAT(tokens[5].as_number(), WithinRel(5.0, 0.001));
    }
}

// ============================================================================
// Mini-Notation Parser Tests
// ============================================================================

TEST_CASE("Mini parser basic patterns", "[mini_parser]") {
    SECTION("single pitch") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4", arena);
        REQUIRE(diags.empty());
        REQUIRE(root != NULL_NODE);
        CHECK(arena[root].type == NodeType::MiniPattern);
        CHECK(arena.child_count(root) == 1);

        NodeIndex atom = arena[root].first_child;
        CHECK(arena[atom].type == NodeType::MiniAtom);
        CHECK(arena[atom].as_mini_atom().kind == Node::MiniAtomKind::Pitch);
        CHECK(arena[atom].as_mini_atom().midi_note == 60);
    }

    SECTION("simple sequence") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4 e4 g4", arena);
        REQUIRE(diags.empty());
        REQUIRE(root != NULL_NODE);
        CHECK(arena.child_count(root) == 3);
    }

    SECTION("rest") {
        AstArena arena;
        auto [root, diags] = parse_mini("~", arena);
        REQUIRE(diags.empty());
        NodeIndex atom = arena[root].first_child;
        CHECK(arena[atom].as_mini_atom().kind == Node::MiniAtomKind::Rest);
    }

    SECTION("elongate") {
        AstArena arena;
        auto [root, diags] = parse_mini("_", arena);
        REQUIRE(diags.empty());
        NodeIndex atom = arena[root].first_child;
        CHECK(arena[atom].as_mini_atom().kind == Node::MiniAtomKind::Elongate);
    }

    SECTION("group subdivision") {
        AstArena arena;
        auto [root, diags] = parse_mini("[a b c]", arena);
        REQUIRE(diags.empty());
        NodeIndex group = arena[root].first_child;
        CHECK(arena[group].type == NodeType::MiniGroup);
        CHECK(arena.child_count(group) == 3);
    }

    SECTION("nested groups") {
        AstArena arena;
        auto [root, diags] = parse_mini("a [b c]", arena);
        REQUIRE(diags.empty());
        CHECK(arena.child_count(root) == 2);

        NodeIndex second = arena[arena[root].first_child].next_sibling;
        CHECK(arena[second].type == NodeType::MiniGroup);
    }

    SECTION("alternating sequence") {
        AstArena arena;
        auto [root, diags] = parse_mini("<a b c>", arena);
        REQUIRE(diags.empty());
        NodeIndex seq = arena[root].first_child;
        CHECK(arena[seq].type == NodeType::MiniSequence);
        CHECK(arena.child_count(seq) == 3);
    }

    SECTION("polyrhythm") {
        AstArena arena;
        auto [root, diags] = parse_mini("[a, b, c]", arena);
        REQUIRE(diags.empty());
        NodeIndex poly = arena[root].first_child;
        CHECK(arena[poly].type == NodeType::MiniPolyrhythm);
        CHECK(arena.child_count(poly) == 3);
    }

    SECTION("euclidean rhythm") {
        AstArena arena;
        auto [root, diags] = parse_mini("bd(3,8)", arena);
        REQUIRE(diags.empty());
        NodeIndex euclid = arena[root].first_child;
        CHECK(arena[euclid].type == NodeType::MiniEuclidean);
        auto& data = arena[euclid].as_mini_euclidean();
        CHECK(data.hits == 3);
        CHECK(data.steps == 8);
        CHECK(data.rotation == 0);
    }

    SECTION("euclidean with rotation") {
        AstArena arena;
        auto [root, diags] = parse_mini("bd(3,8,2)", arena);
        REQUIRE(diags.empty());
        NodeIndex euclid = arena[root].first_child;
        auto& data = arena[euclid].as_mini_euclidean();
        CHECK(data.hits == 3);
        CHECK(data.steps == 8);
        CHECK(data.rotation == 2);
    }

    SECTION("speed modifier") {
        AstArena arena;
        auto [root, diags] = parse_mini("c*2", arena);
        REQUIRE(diags.empty());
        NodeIndex modified = arena[root].first_child;
        CHECK(arena[modified].type == NodeType::MiniModified);
        auto& mod = arena[modified].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Speed);
        CHECK_THAT(mod.value, WithinRel(2.0f, 0.001f));
    }

    SECTION("repeat modifier") {
        AstArena arena;
        auto [root, diags] = parse_mini("c!3", arena);
        REQUIRE(diags.empty());
        NodeIndex modified = arena[root].first_child;
        auto& mod = arena[modified].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Repeat);
        CHECK_THAT(mod.value, WithinRel(3.0f, 0.001f));
    }

    SECTION("chance modifier with value") {
        AstArena arena;
        auto [root, diags] = parse_mini("c?0.5", arena);
        REQUIRE(diags.empty());
        NodeIndex modified = arena[root].first_child;
        auto& mod = arena[modified].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Chance);
        CHECK_THAT(mod.value, WithinRel(0.5f, 0.001f));
    }

    SECTION("chance modifier without value defaults to 0.5") {
        AstArena arena;
        auto [root, diags] = parse_mini("c?", arena);
        REQUIRE(diags.empty());
        NodeIndex modified = arena[root].first_child;
        auto& mod = arena[modified].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Chance);
        CHECK_THAT(mod.value, WithinRel(0.5f, 0.001f));
    }

    SECTION("chance modifier with different values") {
        AstArena arena;
        auto [root, diags] = parse_mini("c?0.25", arena);
        REQUIRE(diags.empty());
        NodeIndex modified = arena[root].first_child;
        auto& mod = arena[modified].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Chance);
        CHECK_THAT(mod.value, WithinRel(0.25f, 0.001f));
    }

    SECTION("choice operator") {
        AstArena arena;
        auto [root, diags] = parse_mini("a | b | c", arena);
        REQUIRE(diags.empty());
        NodeIndex choice = arena[root].first_child;
        CHECK(arena[choice].type == NodeType::MiniChoice);
        CHECK(arena.child_count(choice) == 3);
    }

    SECTION("polymeter basic") {
        AstArena arena;
        auto [root, diags] = parse_mini("{bd sd hh}", arena);
        REQUIRE(diags.empty());
        NodeIndex poly = arena[root].first_child;
        CHECK(arena[poly].type == NodeType::MiniPolymeter);
        CHECK(arena.child_count(poly) == 3);
        CHECK(arena[poly].as_mini_polymeter().step_count == 0);
    }

    SECTION("polymeter with step count") {
        AstArena arena;
        auto [root, diags] = parse_mini("{bd sd}%5", arena);
        REQUIRE(diags.empty());
        NodeIndex poly = arena[root].first_child;
        CHECK(arena[poly].type == NodeType::MiniPolymeter);
        CHECK(arena.child_count(poly) == 2);
        CHECK(arena[poly].as_mini_polymeter().step_count == 5);
    }

    SECTION("nested polymeter") {
        AstArena arena;
        auto [root, diags] = parse_mini("a {b c} d", arena);
        REQUIRE(diags.empty());
        CHECK(arena.child_count(root) == 3);
        NodeIndex second = arena[arena[root].first_child].next_sibling;
        CHECK(arena[second].type == NodeType::MiniPolymeter);
    }
}

// Regression: in n"…" / v"…" modes the lexer used to route digit-leading tokens
// through lex_note_atom / lex_value_atom unconditionally, swallowing modifier
// arguments. Then `@1.25` after a pitch tokenized as `At` + `PitchToken("1.25")`
// instead of `At` + `Number(1.25)`, and parse_modifiers emitted MP01. Covers all
// five value-taking modifiers (`*`, `/`, `@`, `!`, `?`) × both typed prefixes.
TEST_CASE("Modifiers parse inside typed n\"…\" / v\"…\" patterns", "[mini_parser][regression]") {
    using Catch::Matchers::WithinRel;

    SECTION("@ modifier in note mode (the original repro)") {
        AstArena arena;
        auto [root, diags] = parse_mini("g4@1.25 eb4@2 c4@1.75 ~@8", arena, {},
                                        MiniParseMode::Note);
        REQUIRE(diags.empty());

        // Four children, each a MiniModified wrapping the original atom.
        CHECK(arena.child_count(root) == 4);

        NodeIndex c = arena[root].first_child;
        const float expected[] = {1.25f, 2.0f, 1.75f, 8.0f};
        for (float ev : expected) {
            CHECK(arena[c].type == NodeType::MiniModified);
            auto& mod = arena[c].as_mini_modifier();
            CHECK(mod.modifier_type == Node::MiniModifierType::Weight);
            CHECK_THAT(mod.value, WithinRel(ev, 0.001f));
            c = arena[c].next_sibling;
        }
    }

    SECTION("all value-taking modifiers in note mode") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4*2 e4/2 g4!3 a4?0.5", arena, {},
                                        MiniParseMode::Note);
        REQUIRE(diags.empty());
        REQUIRE(arena.child_count(root) == 4);

        NodeIndex c = arena[root].first_child;
        const Node::MiniModifierType expected_types[] = {
            Node::MiniModifierType::Speed,
            Node::MiniModifierType::Slow,
            Node::MiniModifierType::Repeat,
            Node::MiniModifierType::Chance,
        };
        const float expected_values[] = {2.0f, 2.0f, 3.0f, 0.5f};
        for (std::size_t i = 0; i < 4; ++i) {
            CHECK(arena[c].type == NodeType::MiniModified);
            auto& mod = arena[c].as_mini_modifier();
            CHECK(mod.modifier_type == expected_types[i]);
            CHECK_THAT(mod.value, WithinRel(expected_values[i], 0.001f));
            c = arena[c].next_sibling;
        }
    }

    SECTION("@ modifier after bare-MIDI atom in note mode") {
        AstArena arena;
        auto [root, diags] = parse_mini("60@1.5 64@2", arena, {}, MiniParseMode::Note);
        REQUIRE(diags.empty());
        REQUIRE(arena.child_count(root) == 2);

        NodeIndex c = arena[root].first_child;
        auto& mod0 = arena[c].as_mini_modifier();
        CHECK(mod0.modifier_type == Node::MiniModifierType::Weight);
        CHECK_THAT(mod0.value, WithinRel(1.5f, 0.001f));

        c = arena[c].next_sibling;
        auto& mod1 = arena[c].as_mini_modifier();
        CHECK(mod1.modifier_type == Node::MiniModifierType::Weight);
        CHECK_THAT(mod1.value, WithinRel(2.0f, 0.001f));
    }

    SECTION("modifiers in value mode") {
        AstArena arena;
        auto [root, diags] = parse_mini("0.5@2 0.8*3 1.0?0.5", arena, {},
                                        MiniParseMode::Value);
        REQUIRE(diags.empty());
        REQUIRE(arena.child_count(root) == 3);

        NodeIndex c = arena[root].first_child;
        auto& m0 = arena[c].as_mini_modifier();
        CHECK(m0.modifier_type == Node::MiniModifierType::Weight);
        CHECK_THAT(m0.value, WithinRel(2.0f, 0.001f));

        c = arena[c].next_sibling;
        auto& m1 = arena[c].as_mini_modifier();
        CHECK(m1.modifier_type == Node::MiniModifierType::Speed);
        CHECK_THAT(m1.value, WithinRel(3.0f, 0.001f));

        c = arena[c].next_sibling;
        auto& m2 = arena[c].as_mini_modifier();
        CHECK(m2.modifier_type == Node::MiniModifierType::Chance);
        CHECK_THAT(m2.value, WithinRel(0.5f, 0.001f));
    }
}

// ============================================================================
// Pattern Evaluation Tests
// ============================================================================

TEST_CASE("Pattern evaluation", "[pattern_eval]") {
    SECTION("single note") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 1);
        CHECK(events.events[0].type == PatternEventType::Pitch);
        CHECK(events.events[0].midi_note == 60);
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[0].duration, WithinRel(1.0f, 0.001f));
    }

    SECTION("three note sequence") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4 e4 g4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);

        // Check timing (evenly divided)
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.333f, 0.01f));
        CHECK_THAT(events.events[2].time, WithinRel(0.666f, 0.01f));

        // Check notes
        CHECK(events.events[0].midi_note == 60);
        CHECK(events.events[1].midi_note == 64);
        CHECK(events.events[2].midi_note == 67);
    }

    SECTION("group subdivision") {
        AstArena arena;
        auto [root, diags] = parse_mini("a [b c]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);

        // First element takes first half
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        // Group elements share second half
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(0.75f, 0.001f));
    }

    SECTION("alternating sequence cycles") {
        AstArena arena;
        auto [root, diags] = parse_mini("<c4 e4 g4>", arena);
        REQUIRE(diags.empty());

        // Cycle 0 -> first element
        PatternEventStream events0 = evaluate_pattern(root, arena, 0);
        REQUIRE(events0.size() == 1);
        CHECK(events0.events[0].midi_note == 60);

        // Cycle 1 -> second element
        PatternEventStream events1 = evaluate_pattern(root, arena, 1);
        REQUIRE(events1.size() == 1);
        CHECK(events1.events[0].midi_note == 64);

        // Cycle 2 -> third element
        PatternEventStream events2 = evaluate_pattern(root, arena, 2);
        REQUIRE(events2.size() == 1);
        CHECK(events2.events[0].midi_note == 67);

        // Cycle 3 -> wraps to first
        PatternEventStream events3 = evaluate_pattern(root, arena, 3);
        REQUIRE(events3.size() == 1);
        CHECK(events3.events[0].midi_note == 60);
    }

    SECTION("polyrhythm simultaneous") {
        AstArena arena;
        auto [root, diags] = parse_mini("[c4, e4]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 2);
        // Both at same time
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.0f, 0.001f));
    }

    SECTION("euclidean rhythm") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4(3,8)", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3); // 3 hits

        // Euclidean(3,8) = x..x..x. = hits at 0, 3, 6
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.375f, 0.01f)); // 3/8
        CHECK_THAT(events.events[2].time, WithinRel(0.75f, 0.01f));  // 6/8
    }

    SECTION("repeat modifier - single element pattern") {
        // c4!3 as sole element: 3 copies each taking 1/3 of the cycle
        AstArena arena;
        auto [root, diags] = parse_mini("c4!3", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);

        // Three repeats evenly spaced (each takes 1/3 of cycle)
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.333f, 0.01f));
        CHECK_THAT(events.events[2].time, WithinRel(0.666f, 0.01f));
    }

    SECTION("repeat modifier extends sequence") {
        // a!2 b → 3 elements (a, a, b), each gets 1/3 of time
        // This is different from a*2 b which would be 2 elements
        AstArena arena;
        auto [root, diags] = parse_mini("a!2 b", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);

        // Events at 0, 1/3, 2/3
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.333f, 0.01f));
        CHECK_THAT(events.events[2].time, WithinRel(0.666f, 0.01f));

        // Durations should all be 1/3
        CHECK_THAT(events.events[0].duration, WithinRel(0.333f, 0.01f));
        CHECK_THAT(events.events[1].duration, WithinRel(0.333f, 0.01f));
        CHECK_THAT(events.events[2].duration, WithinRel(0.333f, 0.01f));
    }

    SECTION("weight modifier elongation") {
        // a@2 b c → weights 2,1,1 = total 4
        // a at 0-0.5 (2/4), b at 0.5-0.75 (1/4), c at 0.75-1.0 (1/4)
        AstArena arena;
        auto [root, diags] = parse_mini("a@2 b c", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);

        // Check times
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(0.75f, 0.001f));

        // Check durations
        CHECK_THAT(events.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[2].duration, WithinRel(0.25f, 0.001f));
    }

    SECTION("weight modifier does not affect velocity") {
        // Weight should only affect time, not velocity
        AstArena arena;
        auto [root, diags] = parse_mini("a@2", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 1);

        // Velocity should be default (1.0), not 2.0
        CHECK_THAT(events.events[0].velocity, WithinRel(1.0f, 0.001f));
    }

    SECTION("combined weight and repeat") {
        // a@2!2 b → a with weight 2, repeated twice, plus b
        // Effective: 2 copies of weight-2 element + 1 copy of weight-1 element
        // Weights = 2 + 2 + 1 = 5
        // Times: 0-0.4 (2/5), 0.4-0.8 (2/5), 0.8-1.0 (1/5)
        AstArena arena;
        auto [root, diags] = parse_mini("a@2!2 b", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);

        // Check times
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.4f, 0.01f));
        CHECK_THAT(events.events[2].time, WithinRel(0.8f, 0.01f));

        // Check durations
        CHECK_THAT(events.events[0].duration, WithinRel(0.4f, 0.01f));
        CHECK_THAT(events.events[1].duration, WithinRel(0.4f, 0.01f));
        CHECK_THAT(events.events[2].duration, WithinRel(0.2f, 0.01f));
    }

    SECTION("rest produces rest event") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4 ~ g4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);
        CHECK(events.events[0].type == PatternEventType::Pitch);
        CHECK(events.events[1].type == PatternEventType::Rest);
        CHECK(events.events[2].type == PatternEventType::Pitch);
    }

    SECTION("elongate extends previous note (Tidal-compatible)") {
        // c4 _ e4 → c4 takes 2/3 of cycle (elongated), e4 takes 1/3
        AstArena arena;
        auto [root, diags] = parse_mini("c4 _ e4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 2);  // c4 and e4 only (no elongate event)

        // c4 starts at 0, duration extended from 1/3 to 2/3
        CHECK(events.events[0].type == PatternEventType::Pitch);
        CHECK(events.events[0].midi_note == 60);  // C4
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[0].duration, WithinRel(0.667f, 0.01f));  // 2/3 of cycle

        // e4 at 2/3, normal 1/3 duration
        CHECK(events.events[1].type == PatternEventType::Pitch);
        CHECK(events.events[1].midi_note == 64);  // E4
        CHECK_THAT(events.events[1].time, WithinRel(0.667f, 0.01f));
        CHECK_THAT(events.events[1].duration, WithinRel(0.333f, 0.01f));
    }

    SECTION("multiple elongates extend further") {
        // c4 _ _ e4 → c4 takes 3/4, e4 takes 1/4
        AstArena arena;
        auto [root, diags] = parse_mini("c4 _ _ e4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 2);  // c4 and e4 only

        CHECK(events.events[0].midi_note == 60);  // C4
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[0].duration, WithinRel(0.75f, 0.01f));  // 3/4

        CHECK(events.events[1].midi_note == 64);  // E4
        CHECK_THAT(events.events[1].time, WithinRel(0.75f, 0.01f));
        CHECK_THAT(events.events[1].duration, WithinRel(0.25f, 0.01f));  // 1/4
    }

    SECTION("elongate at start is ignored") {
        // _ c4 e4 → elongate at start has nothing to extend, treated as gap
        AstArena arena;
        auto [root, diags] = parse_mini("_ c4 e4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 2);  // c4 and e4 only

        CHECK(events.events[0].midi_note == 60);
        CHECK_THAT(events.events[0].time, WithinRel(0.333f, 0.01f));

        CHECK(events.events[1].midi_note == 64);
        CHECK_THAT(events.events[1].time, WithinRel(0.667f, 0.01f));
    }

    SECTION("elongate with samples") {
        // bd _ sd → bd takes 2/3, sd takes 1/3
        AstArena arena;
        auto [root, diags] = parse_mini("bd _ sd", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 2);

        CHECK(events.events[0].type == PatternEventType::Sample);
        CHECK(events.events[0].sample_name == "bd");
        CHECK_THAT(events.events[0].duration, WithinRel(0.667f, 0.01f));

        CHECK(events.events[1].sample_name == "sd");
        CHECK_THAT(events.events[1].duration, WithinRel(0.333f, 0.01f));
    }

    SECTION("rest does not extend (only tilde is rest)") {
        // c4 ~ e4 → c4, rest, e4 all take 1/3 each
        AstArena arena;
        auto [root, diags] = parse_mini("c4 ~ e4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);  // c4, rest, e4

        CHECK_THAT(events.events[0].duration, WithinRel(0.333f, 0.01f));
        CHECK(events.events[1].type == PatternEventType::Rest);
        CHECK_THAT(events.events[2].duration, WithinRel(0.333f, 0.01f));
    }

    SECTION("sample events") {
        AstArena arena;
        auto [root, diags] = parse_mini("bd sd bd sd", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 4);
        CHECK(events.events[0].type == PatternEventType::Sample);
        CHECK(events.events[0].sample_name == "bd");
        CHECK(events.events[1].sample_name == "sd");
    }

    SECTION("polymeter basic") {
        AstArena arena;
        auto [root, diags] = parse_mini("{bd sd hh}", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);
        // 3 children = 3 steps at 0.0, 0.333, 0.666
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.333f, 0.01f));
        CHECK_THAT(events.events[2].time, WithinRel(0.666f, 0.01f));
        CHECK(events.events[0].sample_name == "bd");
        CHECK(events.events[1].sample_name == "sd");
        CHECK(events.events[2].sample_name == "hh");
    }

    SECTION("polymeter with step count") {
        AstArena arena;
        auto [root, diags] = parse_mini("{bd sd}%5", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 5);
        // 5 steps over 2 children: bd at 0, 2, 4; sd at 1, 3
        // Times: 0.0, 0.2, 0.4, 0.6, 0.8
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK(events.events[0].sample_name == "bd");
        CHECK_THAT(events.events[1].time, WithinRel(0.2f, 0.01f));
        CHECK(events.events[1].sample_name == "sd");
        CHECK_THAT(events.events[2].time, WithinRel(0.4f, 0.01f));
        CHECK(events.events[2].sample_name == "bd");
        CHECK_THAT(events.events[3].time, WithinRel(0.6f, 0.01f));
        CHECK(events.events[3].sample_name == "sd");
        CHECK_THAT(events.events[4].time, WithinRel(0.8f, 0.01f));
        CHECK(events.events[4].sample_name == "bd");
    }

    SECTION("polymeter single vs subdivision single") {
        // For a standalone pattern, {a b c} and [a b c] should produce same timing
        AstArena arena;
        auto [root_sub, diags1] = parse_mini("[bd sd hh]", arena);
        REQUIRE(diags1.empty());
        auto [root_poly, diags2] = parse_mini("{bd sd hh}", arena);
        REQUIRE(diags2.empty());

        PatternEventStream events_sub = evaluate_pattern(root_sub, arena, 0);
        PatternEventStream events_poly = evaluate_pattern(root_poly, arena, 0);

        REQUIRE(events_sub.size() == events_poly.size());
        for (std::size_t i = 0; i < events_sub.size(); ++i) {
            CHECK_THAT(events_sub.events[i].time, WithinRel(events_poly.events[i].time, 0.01f));
        }
    }
}

// ============================================================================
// Nested Bracket Tests - verify arbitrary nesting of [] and <>
// ============================================================================

TEST_CASE("Nested bracket subdivision", "[pattern_eval][nested]") {

    // ------------------------------------------------------------------
    // [] within [] — 2 levels
    // ------------------------------------------------------------------
    SECTION("[] within [] — 2 levels: a [b [c d]]") {
        AstArena arena;
        auto [root, diags] = parse_mini("a [b [c d]]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 4);

        // a: 0.0, dur 0.5
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[0].duration, WithinRel(0.5f, 0.001f));
        // b: 0.5, dur 0.25
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[1].duration, WithinRel(0.25f, 0.001f));
        // c: 0.75, dur 0.125
        CHECK_THAT(events.events[2].time, WithinRel(0.75f, 0.001f));
        CHECK_THAT(events.events[2].duration, WithinRel(0.125f, 0.001f));
        // d: 0.875, dur 0.125
        CHECK_THAT(events.events[3].time, WithinRel(0.875f, 0.001f));
        CHECK_THAT(events.events[3].duration, WithinRel(0.125f, 0.001f));
    }

    // ------------------------------------------------------------------
    // [] within [] — 3 levels
    // ------------------------------------------------------------------
    SECTION("[] within [] — 3 levels: a [b [c [d e]]]") {
        AstArena arena;
        auto [root, diags] = parse_mini("a [b [c [d e]]]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 5);

        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));       // a
        CHECK_THAT(events.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));       // b
        CHECK_THAT(events.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(0.75f, 0.001f));      // c
        CHECK_THAT(events.events[2].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events.events[3].time, WithinRel(0.875f, 0.001f));     // d
        CHECK_THAT(events.events[3].duration, WithinRel(0.0625f, 0.001f));
        CHECK_THAT(events.events[4].time, WithinRel(0.9375f, 0.001f));    // e
        CHECK_THAT(events.events[4].duration, WithinRel(0.0625f, 0.001f));
    }

    // ------------------------------------------------------------------
    // [] within [] — 4 levels
    // ------------------------------------------------------------------
    SECTION("[] within [] — 4 levels: a [b [c [d [e f]]]]") {
        AstArena arena;
        auto [root, diags] = parse_mini("a [b [c [d [e f]]]]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 6);

        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));        // a
        CHECK_THAT(events.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));        // b
        CHECK_THAT(events.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(0.75f, 0.001f));       // c
        CHECK_THAT(events.events[2].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events.events[3].time, WithinRel(0.875f, 0.001f));      // d
        CHECK_THAT(events.events[3].duration, WithinRel(0.0625f, 0.001f));
        CHECK_THAT(events.events[4].time, WithinRel(0.9375f, 0.001f));     // e
        CHECK_THAT(events.events[4].duration, WithinRel(0.03125f, 0.001f));
        CHECK_THAT(events.events[5].time, WithinRel(0.96875f, 0.001f));    // f
        CHECK_THAT(events.events[5].duration, WithinRel(0.03125f, 0.001f));
    }

    // ------------------------------------------------------------------
    // Symmetric nesting: [a b] [c d]
    // ------------------------------------------------------------------
    SECTION("symmetric nesting: [a b] [c d]") {
        AstArena arena;
        auto [root, diags] = parse_mini("[a b] [c d]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 4);

        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));    // a
        CHECK_THAT(events.events[0].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.25f, 0.001f));   // b
        CHECK_THAT(events.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(0.5f, 0.001f));    // c
        CHECK_THAT(events.events[2].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[3].time, WithinRel(0.75f, 0.001f));   // d
        CHECK_THAT(events.events[3].duration, WithinRel(0.25f, 0.001f));
    }

    // ------------------------------------------------------------------
    // Deep symmetric: [a [b c]] [d [e f]]
    // ------------------------------------------------------------------
    SECTION("deep symmetric: [a [b c]] [d [e f]]") {
        AstArena arena;
        auto [root, diags] = parse_mini("[a [b c]] [d [e f]]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 6);

        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));     // a
        CHECK_THAT(events.events[0].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.25f, 0.001f));    // b
        CHECK_THAT(events.events[1].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(0.375f, 0.001f));   // c
        CHECK_THAT(events.events[2].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events.events[3].time, WithinRel(0.5f, 0.001f));     // d
        CHECK_THAT(events.events[3].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[4].time, WithinRel(0.75f, 0.001f));    // e
        CHECK_THAT(events.events[4].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events.events[5].time, WithinRel(0.875f, 0.001f));   // f
        CHECK_THAT(events.events[5].duration, WithinRel(0.125f, 0.001f));
    }

    // ------------------------------------------------------------------
    // <> within [] — alternate inside group
    // ------------------------------------------------------------------
    SECTION("<> within []: a [b <c d>]") {
        AstArena arena;
        auto [root, diags] = parse_mini("a [b <c d>]", arena);
        REQUIRE(diags.empty());

        // Cycle 0: alternate picks c
        PatternEventStream events0 = evaluate_pattern(root, arena, 0);
        REQUIRE(events0.size() == 3);

        CHECK_THAT(events0.events[0].time, WithinRel(0.0f, 0.001f));    // a
        CHECK_THAT(events0.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events0.events[1].time, WithinRel(0.5f, 0.001f));    // b
        CHECK_THAT(events0.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events0.events[2].time, WithinRel(0.75f, 0.001f));   // c
        CHECK_THAT(events0.events[2].duration, WithinRel(0.25f, 0.001f));

        // Cycle 1: alternate picks d
        PatternEventStream events1 = evaluate_pattern(root, arena, 1);
        REQUIRE(events1.size() == 3);

        CHECK_THAT(events1.events[2].time, WithinRel(0.75f, 0.001f));   // d
        CHECK_THAT(events1.events[2].duration, WithinRel(0.25f, 0.001f));
    }

    // ------------------------------------------------------------------
    // [] within <> — group inside alternate
    // ------------------------------------------------------------------
    SECTION("[] within <>: <a [b c]>") {
        AstArena arena;
        auto [root, diags] = parse_mini("<a [b c]>", arena);
        REQUIRE(diags.empty());

        // Cycle 0: alternate picks a
        PatternEventStream events0 = evaluate_pattern(root, arena, 0);
        REQUIRE(events0.size() == 1);
        CHECK_THAT(events0.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events0.events[0].duration, WithinRel(1.0f, 0.001f));

        // Cycle 1: alternate picks [b c]
        PatternEventStream events1 = evaluate_pattern(root, arena, 1);
        REQUIRE(events1.size() == 2);
        CHECK_THAT(events1.events[0].time, WithinRel(0.0f, 0.001f));    // b
        CHECK_THAT(events1.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events1.events[1].time, WithinRel(0.5f, 0.001f));    // c
        CHECK_THAT(events1.events[1].duration, WithinRel(0.5f, 0.001f));
    }

    // ------------------------------------------------------------------
    // <> within <> — nested alternates
    // ------------------------------------------------------------------
    SECTION("<> within <>: <a <b c>>") {
        AstArena arena;
        auto [root, diags] = parse_mini("<a <b c>>", arena);
        REQUIRE(diags.empty());

        // Cycle 0: outer picks a
        PatternEventStream events0 = evaluate_pattern(root, arena, 0);
        REQUIRE(events0.size() == 1);
        CHECK_THAT(events0.events[0].time, WithinRel(0.0f, 0.001f));

        // Cycle 1: outer picks <b c>, inner cycle determines b or c
        PatternEventStream events1 = evaluate_pattern(root, arena, 1);
        REQUIRE(events1.size() == 1);
        CHECK_THAT(events1.events[0].time, WithinRel(0.0f, 0.001f));
    }

    // ------------------------------------------------------------------
    // Deep mixed nesting (4 levels): a [b <c [d e]>]
    // ------------------------------------------------------------------
    SECTION("deep mixed nesting: a [b <c [d e]>]") {
        AstArena arena;
        auto [root, diags] = parse_mini("a [b <c [d e]>]", arena);
        REQUIRE(diags.empty());

        // Cycle 0: alternate picks c (single atom)
        PatternEventStream events0 = evaluate_pattern(root, arena, 0);
        REQUIRE(events0.size() == 3);
        CHECK_THAT(events0.events[0].time, WithinRel(0.0f, 0.001f));    // a
        CHECK_THAT(events0.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events0.events[1].time, WithinRel(0.5f, 0.001f));    // b
        CHECK_THAT(events0.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events0.events[2].time, WithinRel(0.75f, 0.001f));   // c
        CHECK_THAT(events0.events[2].duration, WithinRel(0.25f, 0.001f));

        // Cycle 1: alternate picks [d e] (group → 2 events subdivided)
        PatternEventStream events1 = evaluate_pattern(root, arena, 1);
        REQUIRE(events1.size() == 4);
        CHECK_THAT(events1.events[0].time, WithinRel(0.0f, 0.001f));    // a
        CHECK_THAT(events1.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events1.events[1].time, WithinRel(0.5f, 0.001f));    // b
        CHECK_THAT(events1.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events1.events[2].time, WithinRel(0.75f, 0.001f));   // d
        CHECK_THAT(events1.events[2].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events1.events[3].time, WithinRel(0.875f, 0.001f));  // e
        CHECK_THAT(events1.events[3].duration, WithinRel(0.125f, 0.001f));
    }

    // ------------------------------------------------------------------
    // 4 levels all []: [[a b] [[c d] [e f]]]
    // ------------------------------------------------------------------
    SECTION("4 levels all []: [[a b] [[c d] [e f]]]") {
        AstArena arena;
        auto [root, diags] = parse_mini("[[a b] [[c d] [e f]]]", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 6);

        // Outer group is the only top-level element → takes full [0,1)
        // Inside: [a b] and [[c d] [e f]] each get 0.5
        // [a b]: a@0.0 dur=0.25, b@0.25 dur=0.25
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[0].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[1].duration, WithinRel(0.25f, 0.001f));
        // [[c d] [e f]]: [c d] at 0.5 dur 0.25, [e f] at 0.75 dur 0.25
        //   c@0.5 dur=0.125, d@0.625 dur=0.125
        CHECK_THAT(events.events[2].time, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[2].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events.events[3].time, WithinRel(0.625f, 0.001f));
        CHECK_THAT(events.events[3].duration, WithinRel(0.125f, 0.001f));
        //   e@0.75 dur=0.125, f@0.875 dur=0.125
        CHECK_THAT(events.events[4].time, WithinRel(0.75f, 0.001f));
        CHECK_THAT(events.events[4].duration, WithinRel(0.125f, 0.001f));
        CHECK_THAT(events.events[5].time, WithinRel(0.875f, 0.001f));
        CHECK_THAT(events.events[5].duration, WithinRel(0.125f, 0.001f));
    }

    // ------------------------------------------------------------------
    // <> within [] within <> — 3-level mixed
    // ------------------------------------------------------------------
    SECTION("<> within [] within <>: <[a <b c>] d>") {
        AstArena arena;
        auto [root, diags] = parse_mini("<[a <b c>] d>", arena);
        REQUIRE(diags.empty());

        // Cycle 0: outer picks [a <b c>]
        // [a <b c>] subdivides: a@0.0 dur=0.5, <b c>@0.5 dur=0.5
        // <b c> cycle 0 → b
        PatternEventStream events0 = evaluate_pattern(root, arena, 0);
        REQUIRE(events0.size() == 2);
        CHECK_THAT(events0.events[0].time, WithinRel(0.0f, 0.001f));    // a
        CHECK_THAT(events0.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events0.events[1].time, WithinRel(0.5f, 0.001f));    // b
        CHECK_THAT(events0.events[1].duration, WithinRel(0.5f, 0.001f));

        // Cycle 1: outer picks d
        PatternEventStream events1 = evaluate_pattern(root, arena, 1);
        REQUIRE(events1.size() == 1);
        CHECK_THAT(events1.events[0].time, WithinRel(0.0f, 0.001f));    // d
        CHECK_THAT(events1.events[0].duration, WithinRel(1.0f, 0.001f));
    }

    // ------------------------------------------------------------------
    // [] within <> within [] within <> — 4-level alternating
    // ------------------------------------------------------------------
    SECTION("4-level: <a [b <c [d e]>]>") {
        AstArena arena;
        auto [root, diags] = parse_mini("<a [b <c [d e]>]>", arena);
        REQUIRE(diags.empty());

        // Cycle 0: outer picks a (0%2=0)
        PatternEventStream events0 = evaluate_pattern(root, arena, 0);
        REQUIRE(events0.size() == 1);
        CHECK_THAT(events0.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events0.events[0].duration, WithinRel(1.0f, 0.001f));

        // Cycle 1: outer picks [b <c [d e]>] (1%2=1)
        // [b <c [d e]>] subdivides: b@0.0 dur=0.5, <c [d e]>@0.5 dur=0.5
        // Inner <c [d e]> uses global cycle 1: 1%2=1 → picks [d e]
        // [d e] subdivides 0.5 into d@0.5 dur=0.25, e@0.75 dur=0.25
        PatternEventStream events1 = evaluate_pattern(root, arena, 1);
        REQUIRE(events1.size() == 3);
        CHECK_THAT(events1.events[0].time, WithinRel(0.0f, 0.001f));    // b
        CHECK_THAT(events1.events[0].duration, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events1.events[1].time, WithinRel(0.5f, 0.001f));    // d
        CHECK_THAT(events1.events[1].duration, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events1.events[2].time, WithinRel(0.75f, 0.001f));   // e
        CHECK_THAT(events1.events[2].duration, WithinRel(0.25f, 0.001f));

        // Cycle 2: outer picks a again (2%2=0)
        PatternEventStream events2 = evaluate_pattern(root, arena, 2);
        REQUIRE(events2.size() == 1);

        // Cycle 4: outer picks [b <c [d e]>] (3%2=1)
        // Inner <c [d e]> uses global cycle 3: 3%2=1 → picks [d e] again
        PatternEventStream events3 = evaluate_pattern(root, arena, 3);
        REQUIRE(events3.size() == 3);
    }
}

// ============================================================================
// Multi-Cycle Pattern Tests
// ============================================================================

TEST_CASE("Multi-cycle pattern evaluation", "[pattern_eval][multi_cycle]") {
    SECTION("count_cycles for atoms") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4", arena);
        REQUIRE(diags.empty());

        std::uint32_t cycles = count_pattern_cycles(root, arena);
        CHECK(cycles == 1);
    }

    SECTION("count_cycles for groups") {
        AstArena arena;
        auto [root, diags] = parse_mini("[a b c d]", arena);
        REQUIRE(diags.empty());

        std::uint32_t cycles = count_pattern_cycles(root, arena);
        CHECK(cycles == 1);  // Groups don't add cycles
    }

    SECTION("count_cycles for sequence <a b c>") {
        AstArena arena;
        auto [root, diags] = parse_mini("<a b c>", arena);
        REQUIRE(diags.empty());

        std::uint32_t cycles = count_pattern_cycles(root, arena);
        CHECK(cycles == 3);  // 3 elements = 3 cycles
    }

    SECTION("count_cycles for slow modifier") {
        // Slow modifier /n stretches TIME within single evaluation,
        // it doesn't require additional cycle evaluations.
        // cycle_span is calculated from max event times after evaluation.
        AstArena arena;
        auto [root, diags] = parse_mini("[a b c d]/2", arena);
        REQUIRE(diags.empty());

        std::uint32_t cycles = count_pattern_cycles(root, arena);
        CHECK(cycles == 1);  // /2 stretches time, doesn't add cycles
    }

    SECTION("count_cycles for nested sequence") {
        AstArena arena;
        auto [root, diags] = parse_mini("<[a b] [c d]>", arena);
        REQUIRE(diags.empty());

        std::uint32_t cycles = count_pattern_cycles(root, arena);
        CHECK(cycles == 2);  // 2 elements in sequence
    }

    SECTION("multi-cycle evaluation for <a b c>") {
        AstArena arena;
        auto [root, diags] = parse_mini("<c4 e4 g4>", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern_multi_cycle(root, arena);

        // Should have 3 events (one per cycle)
        REQUIRE(events.size() == 3);
        CHECK_THAT(events.cycle_span, WithinRel(3.0f, 0.001f));

        // Events at times 0, 1, 2 (one per cycle)
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(1.0f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(2.0f, 0.001f));

        // Check notes
        CHECK(events.events[0].midi_note == 60); // C4
        CHECK(events.events[1].midi_note == 64); // E4
        CHECK(events.events[2].midi_note == 67); // G4
    }

    SECTION("multi-cycle evaluation for [a b c d]/2") {
        AstArena arena;
        auto [root, diags] = parse_mini("[c4 e4 g4 b4]/2", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern_multi_cycle(root, arena);

        // Should have 4 events spanning 2 cycles
        REQUIRE(events.size() == 4);
        CHECK_THAT(events.cycle_span, WithinRel(2.0f, 0.001f));

        // Events at times 0, 0.5, 1.0, 1.5 (normalized to 2 cycles)
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(1.0f, 0.001f));
        CHECK_THAT(events.events[3].time, WithinRel(1.5f, 0.001f));
    }

    SECTION("multi-cycle evaluation for nested <[a b] [c d]>") {
        AstArena arena;
        auto [root, diags] = parse_mini("<[c4 e4] [g4 b4]>", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern_multi_cycle(root, arena);

        // Should have 4 events spanning 2 cycles
        REQUIRE(events.size() == 4);
        CHECK_THAT(events.cycle_span, WithinRel(2.0f, 0.001f));

        // Cycle 0: [c4 e4] at times 0.0, 0.5
        // Cycle 1: [g4 b4] at times 1.0, 1.5
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK(events.events[0].midi_note == 60); // C4
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));
        CHECK(events.events[1].midi_note == 64); // E4
        CHECK_THAT(events.events[2].time, WithinRel(1.0f, 0.001f));
        CHECK(events.events[2].midi_note == 67); // G4
        CHECK_THAT(events.events[3].time, WithinRel(1.5f, 0.001f));
        CHECK(events.events[3].midi_note == 71); // B4
    }

    SECTION("single cycle patterns are unchanged") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4 e4 g4", arena);
        REQUIRE(diags.empty());

        PatternEventStream single = evaluate_pattern(root, arena, 0);
        PatternEventStream multi = evaluate_pattern_multi_cycle(root, arena);

        // Should be identical
        REQUIRE(single.size() == multi.size());
        for (std::size_t i = 0; i < single.size(); ++i) {
            CHECK_THAT(single.events[i].time, WithinRel(multi.events[i].time, 0.001f));
            CHECK(single.events[i].midi_note == multi.events[i].midi_note);
        }
    }

    SECTION("sample pattern with sequence") {
        AstArena arena;
        auto [root, diags] = parse_mini("<bd sd hh>", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern_multi_cycle(root, arena);

        REQUIRE(events.size() == 3);
        CHECK_THAT(events.cycle_span, WithinRel(3.0f, 0.001f));

        CHECK(events.events[0].type == PatternEventType::Sample);
        CHECK(events.events[0].sample_name == "bd");
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));

        CHECK(events.events[1].sample_name == "sd");
        CHECK_THAT(events.events[1].time, WithinRel(1.0f, 0.001f));

        CHECK(events.events[2].sample_name == "hh");
        CHECK_THAT(events.events[2].time, WithinRel(2.0f, 0.001f));
    }

    SECTION("sequence with groups inside") {
        AstArena arena;
        auto [root, diags] = parse_mini("<[bd bd] sn>", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern_multi_cycle(root, arena);

        // Cycle 0: [bd bd] at times 0.0, 0.5
        // Cycle 1: sn at time 1.0
        REQUIRE(events.size() == 3);
        CHECK_THAT(events.cycle_span, WithinRel(2.0f, 0.001f));

        CHECK(events.events[0].sample_name == "bd");
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK(events.events[1].sample_name == "bd");
        CHECK_THAT(events.events[1].time, WithinRel(0.5f, 0.001f));
        CHECK(events.events[2].sample_name == "sn");
        CHECK_THAT(events.events[2].time, WithinRel(1.0f, 0.001f));
    }
}

// ============================================================================
// Chord Symbol Tests
// ============================================================================

TEST_CASE("Mini lexer chord symbols", "[mini][chord]") {
    SECTION("basic chord symbols") {
        auto [tokens, diags] = lex_mini("Am C7 Fmaj7 G");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 5); // 4 chords + eof

        CHECK(tokens[0].type == MiniTokenType::ChordToken);
        CHECK(tokens[0].as_chord().root == "A");
        CHECK(tokens[0].as_chord().quality == "m");

        CHECK(tokens[1].type == MiniTokenType::ChordToken);
        CHECK(tokens[1].as_chord().root == "C");
        CHECK(tokens[1].as_chord().quality == "7");

        CHECK(tokens[2].type == MiniTokenType::ChordToken);
        CHECK(tokens[2].as_chord().root == "F");
        CHECK(tokens[2].as_chord().quality == "maj7");

        CHECK(tokens[3].type == MiniTokenType::ChordToken);
        CHECK(tokens[3].as_chord().root == "G");
        CHECK(tokens[3].as_chord().quality == "");
    }

    SECTION("chord with accidentals") {
        auto [tokens, diags] = lex_mini("Bb F#m");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3); // 2 chords + eof

        CHECK(tokens[0].type == MiniTokenType::ChordToken);
        CHECK(tokens[0].as_chord().root == "Bb");
        CHECK(tokens[0].as_chord().quality == "");

        CHECK(tokens[1].type == MiniTokenType::ChordToken);
        CHECK(tokens[1].as_chord().root == "F#");
        CHECK(tokens[1].as_chord().quality == "m");
    }

    SECTION("pitch with octave vs chord") {
        // "A4" should be pitch (4 is not a chord quality)
        // "Am" should be chord
        // "C7" should be chord (7 is dominant 7th)
        // "Bb5" should be pitch (accidental + digit = pitch)
        auto [tokens, diags] = lex_mini("A4 Am C7 Bb5");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 5);

        CHECK(tokens[0].type == MiniTokenType::PitchToken);
        CHECK(tokens[0].as_pitch().midi_note == 69); // A4

        CHECK(tokens[1].type == MiniTokenType::ChordToken);
        CHECK(tokens[1].as_chord().root == "A");
        CHECK(tokens[1].as_chord().quality == "m");

        CHECK(tokens[2].type == MiniTokenType::ChordToken);
        CHECK(tokens[2].as_chord().root == "C");
        CHECK(tokens[2].as_chord().quality == "7");

        CHECK(tokens[3].type == MiniTokenType::PitchToken);
        CHECK(tokens[3].as_pitch().midi_note == 82); // Bb5
    }

    SECTION("lowercase pitches vs uppercase chords") {
        // "c4" lowercase is pitch, "C" uppercase is chord
        auto [tokens, diags] = lex_mini("c4 C");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 3);

        CHECK(tokens[0].type == MiniTokenType::PitchToken);
        CHECK(tokens[1].type == MiniTokenType::ChordToken);
    }

    SECTION("chord intervals") {
        auto [tokens, diags] = lex_mini("Am");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 2);

        const auto& chord = tokens[0].as_chord();
        // Minor chord: root, m3, p5 = [0, 3, 7]
        REQUIRE(chord.intervals.size() == 3);
        CHECK(chord.intervals[0] == 0);
        CHECK(chord.intervals[1] == 3);
        CHECK(chord.intervals[2] == 7);
    }
}

TEST_CASE("Mini parser chord symbols", "[mini][chord]") {
    SECTION("single chord") {
        AstArena arena;
        auto [root, diags] = parse_mini("Am", arena);
        REQUIRE(diags.empty());
        REQUIRE(root != NULL_NODE);
        CHECK(arena[root].type == NodeType::MiniPattern);
        CHECK(arena.child_count(root) == 1);

        NodeIndex atom = arena[root].first_child;
        CHECK(arena[atom].type == NodeType::MiniAtom);
        CHECK(arena[atom].as_mini_atom().kind == Node::MiniAtomKind::Chord);
        CHECK(arena[atom].as_mini_atom().chord_root == "A");
        CHECK(arena[atom].as_mini_atom().chord_quality == "m");
    }

    SECTION("chord sequence") {
        AstArena arena;
        auto [root, diags] = parse_mini("Am C F G", arena);
        REQUIRE(diags.empty());
        REQUIRE(root != NULL_NODE);
        CHECK(arena.child_count(root) == 4);

        NodeIndex child = arena[root].first_child;
        while (child != NULL_NODE) {
            CHECK(arena[child].as_mini_atom().kind == Node::MiniAtomKind::Chord);
            child = arena[child].next_sibling;
        }
    }

    SECTION("mixed pitch and chord") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4 Am e4 G", arena);
        REQUIRE(diags.empty());
        REQUIRE(root != NULL_NODE);
        CHECK(arena.child_count(root) == 4);

        NodeIndex child1 = arena[root].first_child;
        CHECK(arena[child1].as_mini_atom().kind == Node::MiniAtomKind::Pitch);

        NodeIndex child2 = arena[child1].next_sibling;
        CHECK(arena[child2].as_mini_atom().kind == Node::MiniAtomKind::Chord);

        NodeIndex child3 = arena[child2].next_sibling;
        CHECK(arena[child3].as_mini_atom().kind == Node::MiniAtomKind::Pitch);

        NodeIndex child4 = arena[child3].next_sibling;
        CHECK(arena[child4].as_mini_atom().kind == Node::MiniAtomKind::Chord);
    }
}

TEST_CASE("Pattern evaluation with chords", "[pattern_eval][chord]") {
    SECTION("single chord produces chord event") {
        AstArena arena;
        auto [root, diags] = parse_mini("Am", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 1);
        CHECK(events.events[0].type == PatternEventType::Chord);
        REQUIRE(events.events[0].chord_data.has_value());
        CHECK(events.events[0].chord_data->root == "A");
        CHECK(events.events[0].chord_data->quality == "m");
        CHECK(events.events[0].chord_data->intervals.size() == 3);
    }

    SECTION("chord sequence timing") {
        AstArena arena;
        auto [root, diags] = parse_mini("Am C F G", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 4);

        // Events at 0, 0.25, 0.5, 0.75
        CHECK_THAT(events.events[0].time, WithinRel(0.0f, 0.001f));
        CHECK_THAT(events.events[1].time, WithinRel(0.25f, 0.001f));
        CHECK_THAT(events.events[2].time, WithinRel(0.5f, 0.001f));
        CHECK_THAT(events.events[3].time, WithinRel(0.75f, 0.001f));

        // All are chord events
        for (const auto& event : events.events) {
            CHECK(event.type == PatternEventType::Chord);
        }
    }

    SECTION("mixed pitch and chord sequence") {
        AstArena arena;
        auto [root, diags] = parse_mini("c4 Am e4 G", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 4);

        CHECK(events.events[0].type == PatternEventType::Pitch);
        CHECK(events.events[0].midi_note == 60);

        CHECK(events.events[1].type == PatternEventType::Chord);
        CHECK(events.events[1].chord_data->root == "A");

        CHECK(events.events[2].type == PatternEventType::Pitch);
        CHECK(events.events[2].midi_note == 64);

        CHECK(events.events[3].type == PatternEventType::Chord);
        CHECK(events.events[3].chord_data->root == "G");
    }

    SECTION("chord with modifiers") {
        AstArena arena;
        auto [root, diags] = parse_mini("Am!2 G", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 3);

        // Am repeated twice + G = 3 events
        CHECK(events.events[0].type == PatternEventType::Chord);
        CHECK(events.events[0].chord_data->root == "A");
        CHECK(events.events[1].type == PatternEventType::Chord);
        CHECK(events.events[1].chord_data->root == "A");
        CHECK(events.events[2].type == PatternEventType::Chord);
        CHECK(events.events[2].chord_data->root == "G");
    }
}

// ============================================================================
// Velocity Suffix Tests [mini_lexer]
// ============================================================================

TEST_CASE("Pitch velocity suffix", "[mini_lexer]") {
    SECTION("c4:0.8 has velocity 0.8") {
        auto [tokens, diags] = lex_mini("c4:0.8");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK_THAT(static_cast<double>(pitch.velocity), WithinRel(0.8, 0.001));
    }

    SECTION("e4:0.5 has velocity 0.5") {
        auto [tokens, diags] = lex_mini("e4:0.5");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 64);
        CHECK_THAT(static_cast<double>(pitch.velocity), WithinRel(0.5, 0.001));
    }

    SECTION("c4 without suffix has velocity 1.0") {
        auto [tokens, diags] = lex_mini("c4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        CHECK_THAT(static_cast<double>(tokens[0].as_pitch().velocity), WithinRel(1.0, 0.001));
    }

    SECTION("multiple pitches with velocities") {
        auto [tokens, diags] = lex_mini("c4:0.8 e4:0.5 g4");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() >= 4); // 3 pitches + Eof
        CHECK_THAT(static_cast<double>(tokens[0].as_pitch().velocity), WithinRel(0.8, 0.001));
        CHECK_THAT(static_cast<double>(tokens[1].as_pitch().velocity), WithinRel(0.5, 0.001));
        CHECK_THAT(static_cast<double>(tokens[2].as_pitch().velocity), WithinRel(1.0, 0.001));
    }

    SECTION("velocity clamped to 0-1") {
        auto [tokens, diags] = lex_mini("c4:1.5");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        CHECK_THAT(static_cast<double>(tokens[0].as_pitch().velocity), WithinRel(1.0, 0.001));
    }
}

TEST_CASE("Chord velocity suffix", "[mini_lexer]") {
    SECTION("Am:0.5 has velocity 0.5") {
        auto [tokens, diags] = lex_mini("Am:0.5");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::ChordToken);
        CHECK_THAT(static_cast<double>(tokens[0].as_chord().velocity), WithinRel(0.5, 0.001));
    }

    SECTION("C without suffix has velocity 1.0") {
        auto [tokens, diags] = lex_mini("C");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::ChordToken);
        CHECK_THAT(static_cast<double>(tokens[0].as_chord().velocity), WithinRel(1.0, 0.001));
    }
}

// ============================================================================
// Microtonal Pitch Parsing Tests
// ============================================================================

TEST_CASE("Microtonal pitch lexing", "[mini_lexer][microtonal]") {
    SECTION("c^4 — single micro step up") {
        auto [tokens, diags] = lex_mini("c^4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.micro_offset == 1);
    }

    SECTION("c#^4 — sharp + micro up (mixed)") {
        auto [tokens, diags] = lex_mini("c#^4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 61);
        CHECK(pitch.micro_offset == 1);
    }

    SECTION("cbb4 — stacked flats (double flat)") {
        auto [tokens, diags] = lex_mini("cbb4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 58);  // C4 - 2 semitones
        CHECK(pitch.micro_offset == 0);
    }

    SECTION("c+4 — alias for micro up") {
        auto [tokens, diags] = lex_mini("c+4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.micro_offset == 1);
    }

    SECTION("cx4 — double sharp") {
        auto [tokens, diags] = lex_mini("cx4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 62);  // C4 + 2 semitones
        CHECK(pitch.micro_offset == 0);
    }

    SECTION("c^^4 — stacked micro ups") {
        auto [tokens, diags] = lex_mini("c^^4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.micro_offset == 2);
    }

    SECTION("cvv4 — stacked micro downs") {
        auto [tokens, diags] = lex_mini("cvv4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.micro_offset == -2);
    }

    SECTION("c#^v4 — micro up/down cancel") {
        auto [tokens, diags] = lex_mini("c#^v4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 61);
        CHECK(pitch.micro_offset == 0);
    }

    SECTION("c4 backward compat — no micro offset") {
        auto [tokens, diags] = lex_mini("c4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.micro_offset == 0);
    }

    SECTION("f#3 backward compat — no micro offset") {
        auto [tokens, diags] = lex_mini("f#3");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 54);
        CHECK(pitch.micro_offset == 0);
    }

    SECTION("bd sample not affected by microtonal") {
        auto [tokens, diags] = lex_mini("bd");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::SampleToken);
        CHECK(tokens[0].as_sample().name == "bd");
    }

    SECTION("cv4 — micro down with octave") {
        auto [tokens, diags] = lex_mini("cv4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.micro_offset == -1);
    }

    SECTION("microtonal pitch with velocity suffix") {
        auto [tokens, diags] = lex_mini("c^4:0.5");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& pitch = tokens[0].as_pitch();
        CHECK(pitch.midi_note == 60);
        CHECK(pitch.micro_offset == 1);
        CHECK_THAT(static_cast<double>(pitch.velocity), WithinRel(0.5, 0.001));
    }

    SECTION("microtonal pitches in sequence") {
        auto [tokens, diags] = lex_mini("c^4 ev4 g4");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() >= 4); // 3 pitches + Eof
        CHECK(tokens[0].as_pitch().micro_offset == 1);
        CHECK(tokens[1].as_pitch().micro_offset == -1);
        CHECK(tokens[2].as_pitch().micro_offset == 0);
    }
}

TEST_CASE("Microtonal pitch parsing and evaluation", "[mini_parser][microtonal]") {
    SECTION("micro_offset threaded to AST") {
        AstArena arena;
        auto [root, diags] = parse_mini("c^4", arena);
        REQUIRE(diags.empty());
        REQUIRE(root != NULL_NODE);

        NodeIndex atom = arena[root].first_child;
        CHECK(arena[atom].type == NodeType::MiniAtom);
        CHECK(arena[atom].as_mini_atom().kind == Node::MiniAtomKind::Pitch);
        CHECK(arena[atom].as_mini_atom().midi_note == 60);
        CHECK(arena[atom].as_mini_atom().micro_offset == 1);
    }

    SECTION("micro_offset threaded to PatternEvent") {
        AstArena arena;
        auto [root, diags] = parse_mini("c^^4 ev4", arena);
        REQUIRE(diags.empty());

        PatternEventStream events = evaluate_pattern(root, arena, 0);
        REQUIRE(events.size() == 2);
        CHECK(events.events[0].midi_note == 60);
        CHECK(events.events[0].micro_offset == 2);
        CHECK(events.events[1].midi_note == 64);
        CHECK(events.events[1].micro_offset == -1);
    }
}

// ============================================================================
// TuningContext and parse_tuning Tests
// ============================================================================

TEST_CASE("TuningContext resolve_hz", "[tuning]") {
    SECTION("12-EDO default: no micro_offset is standard pitch") {
        TuningContext tuning{12};
        // A4 = 440Hz
        CHECK_THAT(static_cast<double>(tuning.resolve_hz(69, 0)),
                   WithinRel(440.0, 0.001));
        // C4 = ~261.6Hz
        CHECK_THAT(static_cast<double>(tuning.resolve_hz(60, 0)),
                   WithinRel(261.626, 0.001));
    }

    SECTION("12-EDO: micro_offset=+1 is one semitone (100 cents)") {
        TuningContext tuning{12};
        // A4 + 1 micro step in 12-EDO = A#4/Bb4 = ~466.16 Hz
        float a4_up = tuning.resolve_hz(69, 1);
        float asharp4 = tuning.resolve_hz(70, 0);
        CHECK_THAT(static_cast<double>(a4_up), WithinRel(static_cast<double>(asharp4), 0.001));
    }

    SECTION("24-EDO: micro_offset=+1 is quarter tone (50 cents)") {
        TuningContext tuning{24};
        float hz = tuning.resolve_hz(69, 1);
        // 50 cents above A4 = 440 * 2^(50/1200) ≈ 452.89
        CHECK_THAT(static_cast<double>(hz), WithinRel(452.893, 0.01));
    }

    SECTION("31-EDO: micro_offset=+1 is ~38.7 cents") {
        TuningContext tuning{31};
        float hz = tuning.resolve_hz(69, 1);
        // 1200/31 ≈ 38.71 cents above A4 = 440 * 2^(38.71/1200) ≈ 449.90
        double expected = 440.0 * std::pow(2.0, (1200.0 / 31.0) / 1200.0);
        CHECK_THAT(static_cast<double>(hz), WithinRel(expected, 0.001));
    }

    SECTION("no micro_offset: identical to standard formula") {
        TuningContext tuning12{12};
        TuningContext tuning31{31};
        // Without micro_offset, all EDO systems produce the same Hz
        CHECK_THAT(static_cast<double>(tuning12.resolve_hz(60, 0)),
                   WithinRel(static_cast<double>(tuning31.resolve_hz(60, 0)), 0.001));
        CHECK_THAT(static_cast<double>(tuning12.resolve_hz(72, 0)),
                   WithinRel(static_cast<double>(tuning31.resolve_hz(72, 0)), 0.001));
    }
}

TEST_CASE("parse_tuning", "[tuning]") {
    SECTION("standard EDO formats") {
        auto t12 = parse_tuning("12edo");
        REQUIRE(t12.has_value());
        CHECK(t12->divisions == 12);

        auto t24 = parse_tuning("24edo");
        REQUIRE(t24.has_value());
        CHECK(t24->divisions == 24);

        auto t31 = parse_tuning("31edo");
        REQUIRE(t31.has_value());
        CHECK(t31->divisions == 31);
    }

    SECTION("dash format") {
        auto t = parse_tuning("31-edo");
        REQUIRE(t.has_value());
        CHECK(t->divisions == 31);

        auto t2 = parse_tuning("53-EDO");
        REQUIRE(t2.has_value());
        CHECK(t2->divisions == 53);
    }

    SECTION("invalid tunings return nullopt") {
        CHECK_FALSE(parse_tuning("foo").has_value());
        CHECK_FALSE(parse_tuning("").has_value());
        CHECK_FALSE(parse_tuning("edo").has_value());
        CHECK_FALSE(parse_tuning("0edo").has_value());
    }
}

// ============================================================================
// tune() end-to-end compilation tests [tuning]
// ============================================================================

#include "akkado/akkado.hpp"

static bool has_diagnostic_code(const std::vector<akkado::Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& d : diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

TEST_CASE("tune() compiles end-to-end", "[tuning]") {
    SECTION("basic tune() compiles successfully") {
        auto result = akkado::compile(R"(tune("24edo", pat("[c4 c^4]")))");
        INFO("Diagnostics:");
        for (const auto& d : result.diagnostics) {
            INFO("  " << d.code << ": " << d.message);
        }
        CHECK(result.success);
    }

    SECTION("tune() in full pipeline with pipe") {
        auto result = akkado::compile(R"(tune("31edo", pat("a4 a^4 a^^4")) |> ((f) -> osc("sin", f)) |> out(%, %))");
        INFO("Diagnostics:");
        for (const auto& d : result.diagnostics) {
            INFO("  " << d.code << ": " << d.message);
        }
        CHECK(result.success);
    }

    SECTION("invalid tuning name produces error") {
        auto result = akkado::compile(R"(tune("invalid", pat("c4")))");
        CHECK_FALSE(result.success);
    }

    SECTION("non-pattern second arg produces error") {
        auto result = akkado::compile(R"(tune("24edo", 42))");
        CHECK_FALSE(result.success);
    }
}

// Helper: extract emitted event Hz values from a compiled tune() pattern.
// Returns the values[0] (frequency) of every event in the first SequenceProgram.
static std::vector<float> extract_event_hz(const akkado::CompileResult& result) {
    std::vector<float> hz;
    for (const auto& init : result.state_inits) {
        if (init.type != akkado::StateInitData::Type::SequenceProgram) continue;
        for (const auto& events : init.sequence_events) {
            for (const auto& ev : events) hz.push_back(ev.values[0]);
        }
        break;  // first sequence-program init is enough
    }
    return hz;
}

TEST_CASE("tune() emits correct Hz for microtonal notes", "[tuning][hz_roundtrip]") {
    using Catch::Matchers::WithinRel;

    SECTION("12-EDO default: c^4 == c#4 (one semitone)") {
        auto result = akkado::compile(R"(pat("[c4 c^4 c#4]"))");
        REQUIRE(result.success);
        auto hz = extract_event_hz(result);
        REQUIRE(hz.size() == 3);
        // c4 = 60, c#4 = 61; in 12-EDO, c^4 with micro=+1 step (100¢) == c#4
        CHECK_THAT(static_cast<double>(hz[1]),
                   WithinRel(static_cast<double>(hz[2]), 0.001));
    }

    SECTION("24-EDO: c^4 is a quarter tone above c4") {
        auto result = akkado::compile(R"(tune("24edo", pat("[c4 c^4]")))");
        REQUIRE(result.success);
        auto hz = extract_event_hz(result);
        REQUIRE(hz.size() == 2);
        // 24-EDO step = 50¢ → ratio = 2^(50/1200) ≈ 1.02930
        double ratio = static_cast<double>(hz[1]) / static_cast<double>(hz[0]);
        CHECK_THAT(ratio, WithinRel(std::pow(2.0, 50.0 / 1200.0), 0.001));
    }

    SECTION("31-EDO: c^4 step is ~38.7¢") {
        auto result = akkado::compile(R"(tune("31edo", pat("[c4 c^4]")))");
        REQUIRE(result.success);
        auto hz = extract_event_hz(result);
        REQUIRE(hz.size() == 2);
        double ratio = static_cast<double>(hz[1]) / static_cast<double>(hz[0]);
        CHECK_THAT(ratio, WithinRel(std::pow(2.0, 1200.0 / 31.0 / 1200.0), 0.001));
    }

    SECTION("d alias produces -1 micro_offset (sesquiflat cbd4)") {
        auto result = akkado::compile(R"(tune("24edo", pat("[c4 cbd4]")))");
        REQUIRE(result.success);
        auto hz = extract_event_hz(result);
        REQUIRE(hz.size() == 2);
        // cbd4 = c with std=-1 (b flat) and micro=-1 (d alias) in 24-EDO
        // = c4 - 100¢ - 50¢ = -150¢ from c4
        double ratio = static_cast<double>(hz[1]) / static_cast<double>(hz[0]);
        CHECK_THAT(ratio, WithinRel(std::pow(2.0, -150.0 / 1200.0), 0.001));
    }

    SECTION("\\ alias produces -1 micro_offset") {
        auto result = akkado::compile(R"(tune("24edo", pat("[c\\4 c4]")))");
        REQUIRE(result.success);
        auto hz = extract_event_hz(result);
        REQUIRE(hz.size() == 2);
        double ratio = static_cast<double>(hz[0]) / static_cast<double>(hz[1]);
        CHECK_THAT(ratio, WithinRel(std::pow(2.0, -50.0 / 1200.0), 0.001));
    }
}

// ============================================================================
// PRD §4.3 — full stacking-strategy table coverage [microtonal]
// ============================================================================

TEST_CASE("PRD §4.3 stacking table", "[mini_lexer][microtonal][prd_4_3]") {
    auto check = [](const char* input, int expected_std, int expected_micro) {
        auto [tokens, diags] = lex_mini(input);
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() >= 1);
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        const auto& p = tokens[0].as_pitch();
        // accidental_std is folded into midi_note: c4=60, +1=61, -1=59, ...
        CHECK(static_cast<int>(p.midi_note) - 60 == expected_std);
        CHECK(static_cast<int>(p.micro_offset) == expected_micro);
    };

    SECTION("c#4: standard sharp (+1, 0)")     { check("c#4",  +1, 0); }
    SECTION("c^4: pure micro step (0, +1)")    { check("c^4",  0, +1); }
    SECTION("c#^4: sharp + micro (+1, +1)")    { check("c#^4", +1, +1); }
    SECTION("cbb4: stacked flats (-2, 0)")     { check("cbb4", -2, 0); }
    SECTION("cbd4: sesquiflat (-1, -1)")       { check("cbd4", -1, -1); }
    SECTION("c+4: + alias (0, +1)")            { check("c+4",  0, +1); }
    SECTION("cd4: d alias (0, -1)")            { check("cd4",  0, -1); }
    SECTION("cx4: double sharp (+2, 0)")       { check("cx4",  +2, 0); }
    SECTION("c\\4: \\ alias (0, -1)")          { check("c\\4", 0, -1); }
}

TEST_CASE("d alias does not break sample tokens", "[mini_lexer][microtonal][regression]") {
    SECTION("bd is still a sample") {
        auto [tokens, diags] = lex_mini("bd");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::SampleToken);
        CHECK(tokens[0].as_sample().name == "bd");
    }
    SECTION("sd is still a sample") {
        auto [tokens, diags] = lex_mini("sd");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::SampleToken);
        CHECK(tokens[0].as_sample().name == "sd");
    }
    SECTION("bd:2 (sample variant) is still a sample") {
        auto [tokens, diags] = lex_mini("bd:2");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::SampleToken);
        CHECK(tokens[0].as_sample().name == "bd");
        CHECK(tokens[0].as_sample().variant == 2);
    }
    SECTION("standalone 'd' is note D (PRD §4.2: d starting a token is pitch)") {
        auto [tokens, diags] = lex_mini("d");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        // D4 default octave; no micro shift
        CHECK(tokens[0].as_pitch().midi_note == 62);
        CHECK(tokens[0].as_pitch().micro_offset == 0);
    }
    SECTION("'d4' starting a token is note D, not an alias") {
        auto [tokens, diags] = lex_mini("d4");
        REQUIRE(diags.empty());
        REQUIRE(tokens[0].type == MiniTokenType::PitchToken);
        // D4 = MIDI 62; no micro shift
        CHECK(tokens[0].as_pitch().midi_note == 62);
        CHECK(tokens[0].as_pitch().micro_offset == 0);
    }
}

// ============================================================================
// JI and Bohlen-Pierce tuning support [tuning]
// ============================================================================

TEST_CASE("parse_tuning recognizes ji and bp presets", "[tuning][ji_bp]") {
    auto ji = parse_tuning("ji");
    REQUIRE(ji.has_value());
    CHECK(ji->kind == TuningContext::Kind::JI);

    auto bp = parse_tuning("bp");
    REQUIRE(bp.has_value());
    CHECK(bp->kind == TuningContext::Kind::BP);
    CHECK(bp->divisions == 13);
}

TEST_CASE("JI tuning resolves ratios from 5-limit symmetric scale", "[tuning][ji]") {
    using Catch::Matchers::WithinRel;
    TuningContext ji;
    ji.kind = TuningContext::Kind::JI;

    // C4 in JI is anchored at 12-EDO C4 ≈ 261.626 Hz (1/1)
    float c4_hz = 440.0f * std::pow(2.0f, (60.0f - 69.0f) / 12.0f);
    CHECK_THAT(static_cast<double>(ji.resolve_hz(60, 0)),
               WithinRel(static_cast<double>(c4_hz), 0.001));

    // E4 = MIDI 64, JI ratio 5/4 above C4
    CHECK_THAT(static_cast<double>(ji.resolve_hz(64, 0)),
               WithinRel(static_cast<double>(c4_hz) * 5.0 / 4.0, 0.001));

    // G4 = MIDI 67, JI ratio 3/2 above C4
    CHECK_THAT(static_cast<double>(ji.resolve_hz(67, 0)),
               WithinRel(static_cast<double>(c4_hz) * 3.0 / 2.0, 0.001));

    // C5 = MIDI 72, exactly one octave (2/1) above C4
    CHECK_THAT(static_cast<double>(ji.resolve_hz(72, 0)),
               WithinRel(static_cast<double>(c4_hz) * 2.0, 0.001));

    // micro_offset shifts within the JI scale: c4 + micro=+1 lands on c#4 ratio (16/15)
    CHECK_THAT(static_cast<double>(ji.resolve_hz(60, 1)),
               WithinRel(static_cast<double>(c4_hz) * 16.0 / 15.0, 0.001));
}

TEST_CASE("BP tuning uses 13 equal divisions of the tritave", "[tuning][bp]") {
    using Catch::Matchers::WithinRel;
    TuningContext bp{TuningContext::Kind::BP, 13, 1901.9550008653875f};

    // BP step in cents
    float step = bp.step_cents();
    CHECK_THAT(static_cast<double>(step), WithinRel(1901.955 / 13.0, 0.001));

    // midi=60 (root) → C4_hz; +1 BP step = C4 * 2^(step/1200)
    float c4_hz = 440.0f * std::pow(2.0f, (60.0f - 69.0f) / 12.0f);
    CHECK_THAT(static_cast<double>(bp.resolve_hz(60, 0)),
               WithinRel(static_cast<double>(c4_hz), 0.001));

    CHECK_THAT(static_cast<double>(bp.resolve_hz(60, 1)),
               WithinRel(static_cast<double>(c4_hz) * std::pow(2.0, step / 1200.0), 0.001));

    // 13 BP steps = one tritave (3:1)
    CHECK_THAT(static_cast<double>(bp.resolve_hz(60, 13)),
               WithinRel(static_cast<double>(c4_hz) * 3.0, 0.001));
}

TEST_CASE("tune(\"ji\") and tune(\"bp\") compile end-to-end", "[tuning][ji_bp]") {
    SECTION("ji compiles") {
        auto result = akkado::compile(R"(tune("ji", pat("c4 e4 g4")) |> ((f) -> osc("sin", f)) |> out(%, %))");
        CHECK(result.success);
    }
    SECTION("bp compiles") {
        auto result = akkado::compile(R"(tune("bp", pat("c4 c^4 c^^4")) |> ((f) -> osc("sin", f)) |> out(%, %))");
        CHECK(result.success);
    }
}

// ============================================================================
// Curve Lexer Tests [curve_lexer]
// ============================================================================

TEST_CASE("Curve mode lexes level characters", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("_ . - ^ '", {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(tokens.size() == 6); // 5 levels + Eof
    CHECK(tokens[0].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[0].value).value == 0.0f);
    CHECK(tokens[1].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[1].value).value == 0.25f);
    CHECK(tokens[2].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[2].value).value == 0.5f);
    CHECK(tokens[3].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[3].value).value == 0.75f);
    CHECK(tokens[4].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(tokens[4].value).value == 1.0f);
}

TEST_CASE("Curve mode lexes ramp characters", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("/ \\", {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].type == MiniTokenType::CurveRamp);
    CHECK(tokens[1].type == MiniTokenType::CurveRamp);
}

TEST_CASE("Curve mode lexes smooth modifier", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("~", {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].type == MiniTokenType::CurveSmooth);
}

TEST_CASE("Curve mode _ is CurveLevel not Elongate", "[curve_lexer]") {
    auto [std_tokens, std_diags] = lex_mini("_", {}, false, false);
    CHECK(std_tokens[0].type == MiniTokenType::Elongate);
    auto [curve_tokens, curve_diags] = lex_mini("_", {}, false, true);
    CHECK(curve_tokens[0].type == MiniTokenType::CurveLevel);
    CHECK(std::get<MiniCurveLevelData>(curve_tokens[0].value).value == 0.0f);
}

TEST_CASE("Curve mode ~ is CurveSmooth not Rest", "[curve_lexer]") {
    auto [std_tokens, std_diags] = lex_mini("~", {}, false, false);
    CHECK(std_tokens[0].type == MiniTokenType::Rest);
    auto [curve_tokens, curve_diags] = lex_mini("~", {}, false, true);
    CHECK(curve_tokens[0].type == MiniTokenType::CurveSmooth);
}

TEST_CASE("Curve mode / disambiguation", "[curve_lexer]") {
    auto [tokens1, diags1] = lex_mini("_/2", {}, false, true);
    REQUIRE(diags1.empty());
    CHECK(tokens1[0].type == MiniTokenType::CurveLevel);
    CHECK(tokens1[1].type == MiniTokenType::Slash);
    CHECK(tokens1[2].type == MiniTokenType::Number);

    auto [tokens2, diags2] = lex_mini("_/'", {}, false, true);
    REQUIRE(diags2.empty());
    CHECK(tokens2[0].type == MiniTokenType::CurveLevel);
    CHECK(tokens2[1].type == MiniTokenType::CurveRamp);
    CHECK(tokens2[2].type == MiniTokenType::CurveLevel);
}

TEST_CASE("Curve mode grouping and modifiers still work", "[curve_lexer]") {
    auto [tokens, diags] = lex_mini("[_'] *4", {}, false, true);
    REQUIRE(diags.empty());
    CHECK(tokens[0].type == MiniTokenType::LBracket);
    CHECK(tokens[1].type == MiniTokenType::CurveLevel);
    CHECK(tokens[2].type == MiniTokenType::CurveLevel);
    CHECK(tokens[3].type == MiniTokenType::RBracket);
    CHECK(tokens[4].type == MiniTokenType::Star);
}

TEST_CASE("Curve mode mini_token_type_name", "[curve_lexer]") {
    CHECK(mini_token_type_name(MiniTokenType::CurveLevel) == "CurveLevel");
    CHECK(mini_token_type_name(MiniTokenType::CurveRamp) == "CurveRamp");
    CHECK(mini_token_type_name(MiniTokenType::CurveSmooth) == "CurveSmooth");
}

// ============================================================================
// Curve Parser Tests [curve_parser]
// ============================================================================

TEST_CASE("Parse basic curve levels", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_ ' -", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(root != NULL_NODE);
    CHECK(arena[root].type == NodeType::MiniPattern);
    REQUIRE(arena.child_count(root) == 3);

    NodeIndex c0 = arena[root].first_child;
    CHECK(arena[c0].type == NodeType::MiniAtom);
    CHECK(arena[c0].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c0].as_mini_atom().curve_value == 0.0f);
    CHECK(arena[c0].as_mini_atom().curve_smooth == false);

    NodeIndex c1 = arena[c0].next_sibling;
    CHECK(arena[c1].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c1].as_mini_atom().curve_value == 1.0f);

    NodeIndex c2 = arena[c1].next_sibling;
    CHECK(arena[c2].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c2].as_mini_atom().curve_value == 0.5f);
}

TEST_CASE("Parse curve ramps", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_/'", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 3);

    NodeIndex c0 = arena[root].first_child;
    CHECK(arena[c0].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    NodeIndex c1 = arena[c0].next_sibling;
    CHECK(arena[c1].as_mini_atom().kind == Node::MiniAtomKind::CurveRamp);
    NodeIndex c2 = arena[c1].next_sibling;
    CHECK(arena[c2].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c2].as_mini_atom().curve_value == 1.0f);
}

TEST_CASE("Parse smooth modifier ~", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_~'", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 2);

    NodeIndex c0 = arena[root].first_child;
    CHECK(arena[c0].as_mini_atom().curve_smooth == false);
    NodeIndex c1 = arena[c0].next_sibling;
    CHECK(arena[c1].as_mini_atom().kind == Node::MiniAtomKind::CurveLevel);
    CHECK(arena[c1].as_mini_atom().curve_value == 1.0f);
    CHECK(arena[c1].as_mini_atom().curve_smooth == true);
}

TEST_CASE("Parse curve with grouping", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("[_'] __", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 3);
    NodeIndex group = arena[root].first_child;
    CHECK(arena[group].type == NodeType::MiniGroup);
    REQUIRE(arena.child_count(group) == 2);
}

TEST_CASE("Parse curve with alternation", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("<[_'] ['_]>", arena, {}, false, true);
    REQUIRE(diags.empty());
    REQUIRE(arena.child_count(root) == 1);
    NodeIndex seq = arena[root].first_child;
    CHECK(arena[seq].type == NodeType::MiniSequence);
    REQUIRE(arena.child_count(seq) == 2);
}

TEST_CASE("Curve ~ before non-level is error", "[curve_parser]") {
    AstArena arena;
    auto [root, diags] = parse_mini("~/", arena, {}, false, true);
    CHECK(!diags.empty());
}

// ============================================================================
// Curve Evaluation Tests [curve_eval]
// ============================================================================

TEST_CASE("Evaluate constant curve", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("____", arena, {}, false, true);
    REQUIRE(diags.empty());
    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 4);
    for (const auto& event : stream.events) {
        CHECK(event.type == PatternEventType::CurveLevel);
        CHECK(event.curve_value == 0.0f);
        CHECK(event.curve_smooth == false);
    }
    CHECK_THAT(stream.events[0].time, WithinRel(0.0f, 0.01f));
    CHECK_THAT(stream.events[0].duration, WithinRel(0.25f, 0.01f));
    CHECK_THAT(stream.events[1].time, WithinRel(0.25f, 0.01f));
    CHECK_THAT(stream.events[2].time, WithinRel(0.5f, 0.01f));
    CHECK_THAT(stream.events[3].time, WithinRel(0.75f, 0.01f));
}

TEST_CASE("Evaluate step curve", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("__''", arena, {}, false, true);
    REQUIRE(diags.empty());
    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 4);
    CHECK(stream.events[0].curve_value == 0.0f);
    CHECK(stream.events[1].curve_value == 0.0f);
    CHECK(stream.events[2].curve_value == 1.0f);
    CHECK(stream.events[3].curve_value == 1.0f);
}

TEST_CASE("Evaluate curve with ramp", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_/'", arena, {}, false, true);
    REQUIRE(diags.empty());
    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 3);
    CHECK(stream.events[0].type == PatternEventType::CurveLevel);
    CHECK(stream.events[0].curve_value == 0.0f);
    CHECK(stream.events[1].type == PatternEventType::CurveRamp);
    CHECK(stream.events[2].type == PatternEventType::CurveLevel);
    CHECK(stream.events[2].curve_value == 1.0f);
}

TEST_CASE("Evaluate curve with smooth modifier", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_~'", arena, {}, false, true);
    REQUIRE(diags.empty());
    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 2);
    CHECK(stream.events[0].curve_smooth == false);
    CHECK(stream.events[1].curve_smooth == true);
    CHECK(stream.events[1].curve_value == 1.0f);
}

TEST_CASE("Evaluate curve with weight modifier", "[curve_eval]") {
    AstArena arena;
    auto [root, diags] = parse_mini("_@3 '", arena, {}, false, true);
    REQUIRE(diags.empty());
    PatternEventStream stream = evaluate_pattern(root, arena, 0);
    REQUIRE(stream.events.size() == 2);
    CHECK_THAT(stream.events[0].duration, WithinRel(0.75f, 0.01f));
    CHECK_THAT(stream.events[1].duration, WithinRel(0.25f, 0.01f));
}

TEST_CASE("t prefix produces Timeline token", "[timeline_prefix]") {
    auto [tokens, lex_diags] = akkado::lex("t\"__''\"");
    REQUIRE(lex_diags.empty());
    REQUIRE(tokens.size() >= 3);
    CHECK(tokens[0].type == TokenType::Timeline);
    CHECK(tokens[1].type == TokenType::String);
}
