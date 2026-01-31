#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/mini_lexer.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/pattern_eval.hpp"

using namespace akkado;
using Catch::Matchers::WithinRel;

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

    SECTION("rest tokens") {
        auto [tokens, diags] = lex_mini("~ _ ~");
        REQUIRE(diags.empty());
        REQUIRE(tokens.size() == 4);
        CHECK(tokens[0].type == MiniTokenType::Rest);
        CHECK(tokens[1].type == MiniTokenType::Rest);
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
