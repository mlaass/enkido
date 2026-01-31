#include <catch2/catch_test_macros.hpp>
#include <akkado/pattern_debug.hpp>
#include <akkado/ast.hpp>
#include <akkado/mini_parser.hpp>
#include <akkado/mini_lexer.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <string>

using namespace akkado;

// =============================================================================
// Helper to parse mini-notation and get AST
// =============================================================================

static std::pair<AstArena, NodeIndex> parse_mini_pattern(const char* pattern) {
    AstArena arena;
    auto [root, diags] = parse_mini(pattern, arena);
    return {std::move(arena), root};
}

// =============================================================================
// serialize_mini_ast_json tests
// =============================================================================

TEST_CASE("Pattern debug: serialize_mini_ast_json", "[pattern_debug]") {
    SECTION("empty/null node returns null") {
        AstArena arena;
        auto json = serialize_mini_ast_json(NULL_NODE, arena);
        CHECK(json == "null");
    }

    SECTION("simple pitch atom") {
        auto [arena, root] = parse_mini_pattern("c4");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"type\":\"MiniPattern\"") != std::string::npos);
        CHECK(json.find("\"type\":\"MiniAtom\"") != std::string::npos);
        CHECK(json.find("\"kind\":\"Pitch\"") != std::string::npos);
        CHECK(json.find("\"midi\":") != std::string::npos);
    }

    SECTION("rest token") {
        auto [arena, root] = parse_mini_pattern("~");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"kind\":\"Rest\"") != std::string::npos);
    }

    SECTION("sample atom") {
        auto [arena, root] = parse_mini_pattern("kick");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"kind\":\"Sample\"") != std::string::npos);
        CHECK(json.find("\"sampleName\":\"kick\"") != std::string::npos);
    }

    SECTION("sample with variant") {
        auto [arena, root] = parse_mini_pattern("kick:2");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"kind\":\"Sample\"") != std::string::npos);
        CHECK(json.find("\"variant\":2") != std::string::npos);
    }

    SECTION("chord") {
        auto [arena, root] = parse_mini_pattern("Am7");  // Chord notation without apostrophe
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"kind\":\"Chord\"") != std::string::npos);
    }

    SECTION("sequence with multiple atoms") {
        auto [arena, root] = parse_mini_pattern("c4 e4 g4");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"type\":\"MiniPattern\"") != std::string::npos);
        CHECK(json.find("\"children\":") != std::string::npos);
    }

    SECTION("nested group") {
        auto [arena, root] = parse_mini_pattern("[c4 e4]");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"type\":\"MiniGroup\"") != std::string::npos);
    }

    SECTION("euclidean pattern") {
        auto [arena, root] = parse_mini_pattern("c4(3,8)");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"type\":\"MiniEuclidean\"") != std::string::npos);
        CHECK(json.find("\"hits\":3") != std::string::npos);
        CHECK(json.find("\"steps\":8") != std::string::npos);
    }

    SECTION("euclidean with rotation") {
        auto [arena, root] = parse_mini_pattern("c4(3,8,2)");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"type\":\"MiniEuclidean\"") != std::string::npos);
        CHECK(json.find("\"rotation\":2") != std::string::npos);
    }

    SECTION("speed modifier") {
        auto [arena, root] = parse_mini_pattern("c4*2");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"type\":\"MiniModified\"") != std::string::npos);
        CHECK(json.find("\"modifier\":\"Speed\"") != std::string::npos);
    }

    SECTION("slow modifier") {
        auto [arena, root] = parse_mini_pattern("c4/2");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"modifier\":\"Slow\"") != std::string::npos);
    }

    SECTION("weight modifier") {
        auto [arena, root] = parse_mini_pattern("c4@0.5");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"modifier\":\"Weight\"") != std::string::npos);
    }

    SECTION("repeat modifier") {
        auto [arena, root] = parse_mini_pattern("c4!3");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"modifier\":\"Repeat\"") != std::string::npos);
    }

    SECTION("chance modifier") {
        auto [arena, root] = parse_mini_pattern("c4?0.5");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"modifier\":\"Chance\"") != std::string::npos);
    }

    SECTION("polymeter") {
        auto [arena, root] = parse_mini_pattern("{c4 e4 g4}%8");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"type\":\"MiniPolymeter\"") != std::string::npos);
        CHECK(json.find("\"stepCount\":8") != std::string::npos);
    }

    SECTION("alternates - MiniSequence") {
        auto [arena, root] = parse_mini_pattern("<c4 e4 g4>");
        auto json = serialize_mini_ast_json(root, arena);
        // Alternates are represented as MiniSequence
        CHECK(json.find("\"type\":\"MiniSequence\"") != std::string::npos);
    }

    SECTION("source location info") {
        auto [arena, root] = parse_mini_pattern("c4");
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"location\":{\"offset\":") != std::string::npos);
        CHECK(json.find("\"length\":") != std::string::npos);
    }

    SECTION("json string escaping") {
        // Test special characters in sample names would be escaped
        auto [arena, root] = parse_mini_pattern("kick");  // Simple case
        auto json = serialize_mini_ast_json(root, arena);
        CHECK(json.find("\"sampleName\":\"kick\"") != std::string::npos);
    }
}

// =============================================================================
// serialize_sequences_json tests
// =============================================================================

TEST_CASE("Pattern debug: serialize_sequences_json", "[pattern_debug]") {
    SECTION("empty sequences") {
        std::vector<cedar::Sequence> sequences;
        std::vector<std::vector<cedar::Event>> events;
        auto json = serialize_sequences_json(sequences, events);
        CHECK(json == "{\"sequences\":[]}");
    }

    SECTION("single sequence with no events") {
        std::vector<cedar::Sequence> sequences;
        cedar::Sequence seq{};
        seq.mode = cedar::SequenceMode::NORMAL;
        seq.duration = 1.0f;
        seq.step = 0;
        seq.events = nullptr;
        seq.num_events = 0;
        sequences.push_back(seq);

        std::vector<std::vector<cedar::Event>> events;
        events.push_back({});  // Empty event vector

        auto json = serialize_sequences_json(sequences, events);
        CHECK(json.find("\"id\":0") != std::string::npos);
        CHECK(json.find("\"mode\":\"NORMAL\"") != std::string::npos);
        CHECK(json.find("\"duration\":1") != std::string::npos);
        CHECK(json.find("\"events\":[]") != std::string::npos);
    }

    SECTION("sequence with DATA event") {
        std::vector<cedar::Sequence> sequences;
        cedar::Sequence seq{};
        seq.mode = cedar::SequenceMode::NORMAL;
        seq.duration = 1.0f;
        seq.step = 0;
        sequences.push_back(seq);

        std::vector<std::vector<cedar::Event>> events;
        std::vector<cedar::Event> seq_events;
        cedar::Event e{};
        e.type = cedar::EventType::DATA;
        e.time = 0.0f;
        e.duration = 0.5f;
        e.chance = 1.0f;
        e.source_offset = 0;
        e.source_length = 2;
        e.num_values = 2;
        e.values[0] = 60.0f;  // MIDI note
        e.values[1] = 0.8f;   // Velocity
        seq_events.push_back(e);
        events.push_back(seq_events);

        auto json = serialize_sequences_json(sequences, events);
        CHECK(json.find("\"type\":\"DATA\"") != std::string::npos);
        CHECK(json.find("\"time\":0") != std::string::npos);
        CHECK(json.find("\"duration\":0.5") != std::string::npos);
        CHECK(json.find("\"chance\":1") != std::string::npos);
        CHECK(json.find("\"sourceOffset\":0") != std::string::npos);
        CHECK(json.find("\"sourceLength\":2") != std::string::npos);
        CHECK(json.find("\"numValues\":2") != std::string::npos);
        CHECK(json.find("\"values\":[") != std::string::npos);
    }

    SECTION("sequence with SUB_SEQ event") {
        std::vector<cedar::Sequence> sequences;
        cedar::Sequence seq{};
        seq.mode = cedar::SequenceMode::NORMAL;
        seq.duration = 1.0f;
        sequences.push_back(seq);

        std::vector<std::vector<cedar::Event>> events;
        std::vector<cedar::Event> seq_events;
        cedar::Event e{};
        e.type = cedar::EventType::SUB_SEQ;
        e.time = 0.0f;
        e.duration = 0.5f;
        e.chance = 1.0f;
        e.seq_id = 1;
        seq_events.push_back(e);
        events.push_back(seq_events);

        auto json = serialize_sequences_json(sequences, events);
        CHECK(json.find("\"type\":\"SUB_SEQ\"") != std::string::npos);
        CHECK(json.find("\"seqId\":1") != std::string::npos);
    }

    SECTION("ALTERNATE sequence mode") {
        std::vector<cedar::Sequence> sequences;
        cedar::Sequence seq{};
        seq.mode = cedar::SequenceMode::ALTERNATE;
        seq.duration = 1.0f;
        sequences.push_back(seq);

        std::vector<std::vector<cedar::Event>> events;
        events.push_back({});

        auto json = serialize_sequences_json(sequences, events);
        CHECK(json.find("\"mode\":\"ALTERNATE\"") != std::string::npos);
    }

    SECTION("RANDOM sequence mode") {
        std::vector<cedar::Sequence> sequences;
        cedar::Sequence seq{};
        seq.mode = cedar::SequenceMode::RANDOM;
        seq.duration = 1.0f;
        sequences.push_back(seq);

        std::vector<std::vector<cedar::Event>> events;
        events.push_back({});

        auto json = serialize_sequences_json(sequences, events);
        CHECK(json.find("\"mode\":\"RANDOM\"") != std::string::npos);
    }

    SECTION("multiple sequences") {
        std::vector<cedar::Sequence> sequences;

        cedar::Sequence seq1{};
        seq1.mode = cedar::SequenceMode::NORMAL;
        seq1.duration = 1.0f;
        sequences.push_back(seq1);

        cedar::Sequence seq2{};
        seq2.mode = cedar::SequenceMode::ALTERNATE;
        seq2.duration = 0.5f;
        sequences.push_back(seq2);

        std::vector<std::vector<cedar::Event>> events;
        events.push_back({});
        events.push_back({});

        auto json = serialize_sequences_json(sequences, events);
        CHECK(json.find("\"id\":0") != std::string::npos);
        CHECK(json.find("\"id\":1") != std::string::npos);
    }

    SECTION("sequence using events from Sequence struct pointer") {
        // Create an event to point to
        static cedar::Event static_event{};
        static_event.type = cedar::EventType::DATA;
        static_event.time = 0.25f;
        static_event.duration = 0.25f;
        static_event.chance = 0.75f;
        static_event.num_values = 1;
        static_event.values[0] = 72.0f;

        std::vector<cedar::Sequence> sequences;
        cedar::Sequence seq{};
        seq.mode = cedar::SequenceMode::NORMAL;
        seq.duration = 1.0f;
        seq.events = &static_event;
        seq.num_events = 1;
        sequences.push_back(seq);

        // Empty events vector - will fall back to seq.events pointer
        std::vector<std::vector<cedar::Event>> events;

        auto json = serialize_sequences_json(sequences, events);
        CHECK(json.find("\"time\":0.25") != std::string::npos);
        CHECK(json.find("\"chance\":0.75") != std::string::npos);
        CHECK(json.find("\"values\":[72") != std::string::npos);
    }
}
