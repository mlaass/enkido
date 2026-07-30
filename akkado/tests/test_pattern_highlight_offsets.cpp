// Regression tests for IDE pattern step-highlighting source offsets.
//
// The web IDE draws the playback head at `pattern_location.offset +
// event.source_offset` for `event.source_length` characters. For that to land
// on the right token, two coordinates must agree exactly:
//   * pattern_location.offset — the document offset stored for the literal,
//   * event.source_offset    — per-event offset, relative to the pattern base.
//
// Several parse_mini() entry points used to disagree by the opening-quote
// character (chord(...) calls, bare-string patterns, chord-in-transform), so
// the highlight drifted one column left. These tests pin the invariant for
// every pattern form: every event maps onto an exact, non-whitespace,
// non-quote token in the original source.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <set>
#include <string>
#include <vector>

using namespace akkado;

namespace {

std::vector<const StateInitData*> seq_inits(const CompileResult& r) {
    std::vector<const StateInitData*> out;
    for (const auto& s : r.program.state_inits) {
        if (s.type == StateInitData::Type::SequenceProgram) out.push_back(&s);
    }
    return out;
}

// Verify every event in `init` maps onto an exact source token and return the
// set of token substrings the highlight would cover.
std::set<std::string> aligned_tokens(const std::string& src,
                                     const StateInitData& init) {
    INFO("pattern_location offset=" << init.pattern_location.offset
         << " length=" << init.pattern_location.length);
    REQUIRE(init.pattern_location.offset + init.pattern_location.length
            <= src.size());

    std::set<std::string> tokens;
    for (const auto& seq : init.sequence_events) {
        for (const auto& e : seq) {
            if (e.source_length == 0) continue;  // synthetic event / rest
            std::size_t abs =
                static_cast<std::size_t>(init.pattern_location.offset)
                + e.source_offset;
            INFO("event abs=" << abs << " len=" << e.source_length);
            REQUIRE(abs + e.source_length <= src.size());
            std::string tok = src.substr(abs, e.source_length);
            // The off-by-one symptom was a highlight starting on the opening
            // quote or a leading space. A correctly placed highlight covers
            // exactly the token, never adjacent whitespace or quotes.
            CHECK(tok.front() != ' ');
            CHECK(tok.front() != '"');
            CHECK(tok.back() != ' ');
            CHECK(tok.back() != '"');
            tokens.insert(tok);
        }
    }
    return tokens;
}

} // namespace

TEST_CASE("Pattern highlight: note-prefix literal offsets land on tokens",
          "[pattern_highlight]") {
    const std::string src = "n\"c4 e4 g4\"";
    auto result = compile(src);
    REQUIRE(result.success);

    auto inits = seq_inits(result);
    REQUIRE(inits.size() == 1);

    // pattern_location must point at the content (the 'c'), not the quote.
    CHECK(inits[0]->pattern_location.offset == src.find("c4"));
    // ...and span the whole pattern content, not just the first token.
    CHECK(inits[0]->pattern_location.length == std::string("c4 e4 g4").size());

    auto tokens = aligned_tokens(src, *inits[0]);
    CHECK(tokens == std::set<std::string>{"c4", "e4", "g4"});
}

TEST_CASE("Pattern highlight: value- and chord-prefix literals",
          "[pattern_highlight]") {
    SECTION("value prefix") {
        const std::string src = "v\"1 2 3\"";
        auto result = compile(src);
        REQUIRE(result.success);
        auto inits = seq_inits(result);
        REQUIRE(inits.size() == 1);
        CHECK(inits[0]->pattern_location.offset == src.find('1'));
        auto tokens = aligned_tokens(src, *inits[0]);
        CHECK(tokens == std::set<std::string>{"1", "2", "3"});
    }

    SECTION("chord prefix") {
        const std::string src = "c\"c4 e4 g4\"";
        auto result = compile(src);
        REQUIRE(result.success);
        auto inits = seq_inits(result);
        REQUIRE(inits.size() == 1);
        CHECK(inits[0]->pattern_location.offset == src.find("c4"));
        auto tokens = aligned_tokens(src, *inits[0]);
        CHECK(tokens == std::set<std::string>{"c4", "e4", "g4"});
    }
}

TEST_CASE("Pattern highlight: chord() call offsets land on tokens",
          "[pattern_highlight]") {
    // chord(...) previously stored pattern_location at the opening quote while
    // events were content-relative — a one-character left drift.
    const std::string src = "chord(\"c4 e4 g4\")";
    auto result = compile(src);
    REQUIRE(result.success);

    auto inits = seq_inits(result);
    REQUIRE(inits.size() == 1);

    CHECK(inits[0]->pattern_location.offset == src.find("c4"));

    auto tokens = aligned_tokens(src, *inits[0]);
    CHECK(tokens == std::set<std::string>{"c4", "e4", "g4"});
}

TEST_CASE("Pattern highlight: leading whitespace inside the quotes",
          "[pattern_highlight]") {
    const std::string src = "n\"  c4 e4\"";
    auto result = compile(src);
    REQUIRE(result.success);

    auto inits = seq_inits(result);
    REQUIRE(inits.size() == 1);

    // Even with leading padding the highlight covers exactly the tokens.
    auto tokens = aligned_tokens(src, *inits[0]);
    CHECK(tokens == std::set<std::string>{"c4", "e4"});
}

TEST_CASE("Pattern highlight: slow() transform keeps offsets",
          "[pattern_highlight]") {
    SECTION("method form") {
        const std::string src = "n\"c4 e4\".slow(2)";
        auto result = compile(src);
        REQUIRE(result.success);
        auto inits = seq_inits(result);
        REQUIRE(inits.size() == 1);
        auto tokens = aligned_tokens(src, *inits[0]);
        CHECK(tokens == std::set<std::string>{"c4", "e4"});
    }

    SECTION("call form") {
        const std::string src = "slow(n\"c4 e4\", 2)";
        auto result = compile(src);
        REQUIRE(result.success);
        auto inits = seq_inits(result);
        REQUIRE(inits.size() == 1);
        CHECK(inits[0]->pattern_location.offset == src.find("c4"));
        auto tokens = aligned_tokens(src, *inits[0]);
        CHECK(tokens == std::set<std::string>{"c4", "e4"});
    }
}

TEST_CASE("Pattern highlight: one literal feeding two instances",
          "[pattern_highlight]") {
    // A single literal reused by two transforms compiles into two sequence
    // states. Both must carry the SAME pattern_location (so the IDE knows it
    // is one source span) but DISTINCT state_ids (so each is polled and drawn
    // independently — "show all active steps").
    const std::string src =
        "f = n\"c4 e4\"\n"
        "slow(f, 2)\n"
        "fast(f, 2)";
    auto result = compile(src);
    REQUIRE(result.success);

    auto inits = seq_inits(result);
    REQUIRE(inits.size() >= 2);

    const std::uint32_t lit_offset =
        static_cast<std::uint32_t>(src.find("c4"));

    std::set<std::uint32_t> state_ids_at_literal;
    for (const auto* init : inits) {
        aligned_tokens(src, *init);  // every instance still aligns
        if (init->pattern_location.offset == lit_offset) {
            state_ids_at_literal.insert(init->state_id);
        }
    }

    // Two transforms over one literal -> two instances sharing its location.
    CHECK(state_ids_at_literal.size() >= 2);
}
