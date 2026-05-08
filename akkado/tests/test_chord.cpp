#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "akkado/chord_parser.hpp"
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cmath>
#include <cstring>
#include <set>

using Catch::Matchers::WithinRel;

TEST_CASE("Chord symbol parsing", "[chord]") {
    SECTION("major triads") {
        auto c = akkado::parse_chord_symbol("C");
        REQUIRE(c.has_value());
        CHECK(c->root == "C");
        CHECK(c->quality == "");
        CHECK(c->intervals == std::vector<int>{0, 4, 7});

        auto g = akkado::parse_chord_symbol("G");
        REQUIRE(g.has_value());
        CHECK(g->root == "G");
        CHECK(g->intervals == std::vector<int>{0, 4, 7});
    }

    SECTION("minor triads") {
        auto am = akkado::parse_chord_symbol("Am");
        REQUIRE(am.has_value());
        CHECK(am->root == "A");
        CHECK(am->quality == "m");
        CHECK(am->intervals == std::vector<int>{0, 3, 7});

        auto dm = akkado::parse_chord_symbol("Dm");
        REQUIRE(dm.has_value());
        CHECK(dm->quality == "m");
    }

    SECTION("seventh chords") {
        auto c7 = akkado::parse_chord_symbol("C7");
        REQUIRE(c7.has_value());
        CHECK(c7->quality == "7");
        CHECK(c7->intervals == std::vector<int>{0, 4, 7, 10});

        auto cmaj7 = akkado::parse_chord_symbol("Cmaj7");
        REQUIRE(cmaj7.has_value());
        CHECK(cmaj7->quality == "maj7");
        CHECK(cmaj7->intervals == std::vector<int>{0, 4, 7, 11});

        auto am7 = akkado::parse_chord_symbol("Am7");
        REQUIRE(am7.has_value());
        CHECK(am7->quality == "m7");
        CHECK(am7->intervals == std::vector<int>{0, 3, 7, 10});
    }

    SECTION("accidentals") {
        auto fsharp = akkado::parse_chord_symbol("F#");
        REQUIRE(fsharp.has_value());
        CHECK(fsharp->root == "F#");

        auto bb = akkado::parse_chord_symbol("Bb");
        REQUIRE(bb.has_value());
        CHECK(bb->root == "Bb");

        auto bbm = akkado::parse_chord_symbol("Bbm");
        REQUIRE(bbm.has_value());
        CHECK(bbm->root == "Bb");
        CHECK(bbm->quality == "m");
    }

    SECTION("diminished and augmented") {
        auto cdim = akkado::parse_chord_symbol("Cdim");
        REQUIRE(cdim.has_value());
        CHECK(cdim->quality == "dim");
        CHECK(cdim->intervals == std::vector<int>{0, 3, 6});

        auto caug = akkado::parse_chord_symbol("Caug");
        REQUIRE(caug.has_value());
        CHECK(caug->quality == "aug");
        CHECK(caug->intervals == std::vector<int>{0, 4, 8});
    }

    SECTION("suspended chords") {
        auto sus4 = akkado::parse_chord_symbol("Csus4");
        REQUIRE(sus4.has_value());
        CHECK(sus4->quality == "sus4");
        CHECK(sus4->intervals == std::vector<int>{0, 5, 7});

        auto sus2 = akkado::parse_chord_symbol("Csus2");
        REQUIRE(sus2.has_value());
        CHECK(sus2->quality == "sus2");
        CHECK(sus2->intervals == std::vector<int>{0, 2, 7});
    }

    SECTION("power chord") {
        auto c5 = akkado::parse_chord_symbol("C5");
        REQUIRE(c5.has_value());
        CHECK(c5->quality == "5");
        CHECK(c5->intervals == std::vector<int>{0, 7});
    }
}

TEST_CASE("Chord parser knows extended and alt-symbol qualities", "[chord]") {
    // Regression: these qualities used to round-trip correctly through
    // parse_chord_symbol but were silently truncated to a major triad by
    // the mini-notation lexer because music_theory.hpp's lookup table was
    // a partial subset of chord_parser's. Both tables are now unified;
    // every quality reachable here must also lex correctly via chord("…")
    // and c"…" (covered separately in test_codegen.cpp).

    auto check = [](const char* sym, const std::string& expected_quality,
                    std::vector<int> expected_intervals) {
        INFO("symbol = " << sym);
        auto c = akkado::parse_chord_symbol(sym);
        REQUIRE(c.has_value());
        CHECK(c->quality == expected_quality);
        CHECK(c->intervals == expected_intervals);
    };

    SECTION("major-7th aliases") {
        check("CM7",  "M7",   {0, 4, 7, 11});
        check("C^",   "^",    {0, 4, 7, 11});
        check("C^7",  "^7",   {0, 4, 7, 11});
        check("Cmaj7","maj7", {0, 4, 7, 11});
    }

    SECTION("minor-9th and major-9th") {
        check("Cm9",  "m9",   {0, 3, 7, 10, 14});
        check("CM9",  "M9",   {0, 4, 7, 11, 14});
        check("Cmaj9","maj9", {0, 4, 7, 11, 14});
    }

    SECTION("sixth chords") {
        check("Cm6",  "m6",   {0, 3, 7, 9});
        check("Cmin6","min6", {0, 3, 7, 9});
    }

    SECTION("11th and 13th") {
        check("C13",  "13",   {0, 4, 7, 10, 14, 21});
        check("Cm11", "m11",  {0, 3, 7, 10, 14, 17});
        check("C11",  "11",   {0, 4, 7, 10, 14, 17});
    }

    SECTION("minor-major 7th aliases") {
        check("CmM7",    "mM7",     {0, 3, 7, 11});
        check("Cm^7",    "m^7",     {0, 3, 7, 11});
        check("Cminmaj7","minmaj7", {0, 3, 7, 11});
    }

    SECTION("alternate symbols") {
        check("C-",   "-",  {0, 3, 7});      // minor
        check("C-7",  "-7", {0, 3, 7, 10});  // minor-7th
        check("C+",   "+",  {0, 4, 8});      // augmented
        check("C+7",  "+7", {0, 4, 8, 10});  // augmented-7th
        check("Co",   "o",  {0, 3, 6});      // diminished
        check("Co7",  "o7", {0, 3, 6, 9});   // diminished-7th
    }

    SECTION("add chords") {
        check("Cadd9","add9", {0, 4, 7, 14});
        check("Cadd2","add2", {0, 2, 4, 7});
    }

    SECTION("sus default") {
        check("Csus", "sus", {0, 5, 7});  // sus alone defaults to sus4
    }

    SECTION("dominant aliases") {
        check("Cdom7","dom7", {0, 4, 7, 10});
    }

    SECTION("half-diminished alternate") {
        check("Cm7b5","m7b5", {0, 3, 6, 10});
        check("C0",   "0",    {0, 3, 6, 10});
    }
}

TEST_CASE("Chord expansion to MIDI", "[chord]") {
    SECTION("C major at octave 4") {
        auto chord = akkado::parse_chord_symbol("C");
        REQUIRE(chord.has_value());
        auto notes = akkado::expand_chord(*chord, 4);
        // C4=60, E4=64, G4=67
        REQUIRE(notes.size() == 3);
        CHECK(notes[0] == 60);
        CHECK(notes[1] == 64);
        CHECK(notes[2] == 67);
    }

    SECTION("A minor at octave 3") {
        auto chord = akkado::parse_chord_symbol("Am");
        REQUIRE(chord.has_value());
        auto notes = akkado::expand_chord(*chord, 3);
        // A3=57, C4=60, E4=64
        REQUIRE(notes.size() == 3);
        CHECK(notes[0] == 57);
        CHECK(notes[1] == 60);
        CHECK(notes[2] == 64);
    }

    SECTION("G7 at octave 4") {
        auto chord = akkado::parse_chord_symbol("G7");
        REQUIRE(chord.has_value());
        auto notes = akkado::expand_chord(*chord, 4);
        // G4=67, B4=71, D5=74, F5=77
        REQUIRE(notes.size() == 4);
        CHECK(notes[0] == 67);
        CHECK(notes[1] == 71);
        CHECK(notes[2] == 74);
        CHECK(notes[3] == 77);
    }
}

TEST_CASE("Chord pattern parsing", "[chord]") {
    SECTION("single chord") {
        auto chords = akkado::parse_chord_pattern("Am");
        REQUIRE(chords.size() == 1);
        CHECK(chords[0].root == "A");
        CHECK(chords[0].quality == "m");
    }

    SECTION("multiple chords") {
        auto chords = akkado::parse_chord_pattern("Am C7 F G");
        REQUIRE(chords.size() == 4);
        CHECK(chords[0].root == "A");
        CHECK(chords[0].quality == "m");
        CHECK(chords[1].root == "C");
        CHECK(chords[1].quality == "7");
        CHECK(chords[2].root == "F");
        CHECK(chords[2].quality == "");
        CHECK(chords[3].root == "G");
        CHECK(chords[3].quality == "");
    }

    SECTION("extra whitespace") {
        auto chords = akkado::parse_chord_pattern("  Am   C7    ");
        REQUIRE(chords.size() == 2);
        CHECK(chords[0].root == "A");
        CHECK(chords[1].root == "C");
    }
}

TEST_CASE("Root to MIDI conversion", "[chord]") {
    SECTION("natural notes at octave 4") {
        CHECK(akkado::root_name_to_midi("C", 4) == 60);
        CHECK(akkado::root_name_to_midi("D", 4) == 62);
        CHECK(akkado::root_name_to_midi("E", 4) == 64);
        CHECK(akkado::root_name_to_midi("F", 4) == 65);
        CHECK(akkado::root_name_to_midi("G", 4) == 67);
        CHECK(akkado::root_name_to_midi("A", 4) == 69);
        CHECK(akkado::root_name_to_midi("B", 4) == 71);
    }

    SECTION("sharps and flats") {
        CHECK(akkado::root_name_to_midi("C#", 4) == 61);
        CHECK(akkado::root_name_to_midi("Db", 4) == 61);
        CHECK(akkado::root_name_to_midi("F#", 4) == 66);
        CHECK(akkado::root_name_to_midi("Bb", 4) == 70);
    }

    SECTION("different octaves") {
        CHECK(akkado::root_name_to_midi("C", 3) == 48);
        CHECK(akkado::root_name_to_midi("C", 5) == 72);
        CHECK(akkado::root_name_to_midi("A", 0) == 21);  // A0 = lowest piano key
    }

    SECTION("lowercase notes") {
        CHECK(akkado::root_name_to_midi("c", 4) == 60);
        CHECK(akkado::root_name_to_midi("a", 4) == 69);
    }
}

// ============================================================================
// Chord without poly() produces E410 error
// ============================================================================

TEST_CASE("chord() without poly() produces error", "[chord][akkado]") {
    SECTION("single chord without poly is error") {
        auto result = akkado::compile("chord(\"Am\")");
        CHECK_FALSE(result.success);
        bool found_e410 = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E410") found_e410 = true;
        }
        CHECK(found_e410);
    }

    SECTION("chord progression without poly is error") {
        auto result = akkado::compile("chord(\"Am C7 F G\")");
        CHECK_FALSE(result.success);
    }

    SECTION("chord with pipe without poly is error") {
        auto result = akkado::compile("chord(\"Am\") |> osc(\"saw\", %) |> out(%, %)");
        CHECK_FALSE(result.success);
    }

    SECTION("chord pattern with pipe without poly is error") {
        auto result = akkado::compile("chord(\"Am C F G\") |> osc(\"saw\", %) |> out(%, %)");
        CHECK_FALSE(result.success);
    }

    SECTION("E410 message includes voice count") {
        auto result = akkado::compile("chord(\"Am\")");
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E410") {
                found = true;
                CHECK(d.message.find("3 voices") != std::string::npos);
                CHECK(d.message.find("poly()") != std::string::npos);
            }
        }
        CHECK(found);
    }

    SECTION("chord produces E410 for progressions too") {
        auto result = akkado::compile("chord(\"Am C F\")");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("map() applies function to each element", "[array][map]") {
    SECTION("map with single-element input") {
        // Single value should just apply function once
        auto result = akkado::compile("map([440], (f) -> osc(\"sin\", f)) |> sum(%) |> out(%, %)");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int osc_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::OSC_SIN) {
                osc_count++;
            }
        }
        CHECK(osc_count == 1);
    }

    SECTION("map over multi-element array") {
        auto result = akkado::compile("map([440, 550, 660], (f) -> osc(\"sin\", f)) |> sum(%) |> out(%, %)");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int osc_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::OSC_SIN) {
                osc_count++;
            }
        }
        CHECK(osc_count == 3);  // 3 oscillators for 3 elements
    }

    SECTION("map over chord without poly is error") {
        auto result = akkado::compile(
            R"(chord("Am") |> mtof(%) |> map(%, (f) -> osc("tri", f)) |> sum(%) |> out(%, %))");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("sum() reduces array to single signal", "[array][sum]") {
    SECTION("sum of single element returns it") {
        auto result = akkado::compile("sum([42])");
        REQUIRE(result.success);

        // Should just be PUSH_CONST(42), no ADD needed
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int add_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::ADD) {
                add_count++;
            }
        }
        CHECK(add_count == 0);  // No ADDs for single element
    }

    SECTION("sum of multiple elements chains ADDs") {
        auto result = akkado::compile("sum([1, 2, 3])");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int add_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::ADD) {
                add_count++;
            }
        }
        CHECK(add_count == 2);  // (1+2)+3 = 2 ADDs
    }

    SECTION("sum with map over chord without poly is error") {
        auto result = akkado::compile(
            R"(chord("C") |> mtof(%) |> map(%, (f) -> osc("sin", f)) |> sum(%) |> out(%, %))");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("mtof() with chord without poly is error", "[array][mtof]") {
    SECTION("mtof on chord without poly is error") {
        auto result = akkado::compile("chord(\"Am\") |> mtof(%)");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("map() voices have unique state_ids", "[array][map]") {
    // Uses array literal (not chord pattern) so no poly() needed
    auto result = akkado::compile(
        R"([261.6, 329.6, 392.0] |> map(%, (f) -> osc("sin", f)) |> sum(%) |> out(%, %))");
    REQUIRE(result.success);

    auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

    std::set<std::uint32_t> state_ids;
    for (std::size_t i = 0; i < count; ++i) {
        if (insts[i].opcode == cedar::Opcode::OSC_SIN) {
            state_ids.insert(insts[i].state_id);
        }
    }

    CHECK(state_ids.size() == 3);  // 3 unique state_ids
}

TEST_CASE("chord without poly produces error in pipeline", "[chord][polyphony]") {
    auto result = akkado::compile(R"(
        chord("Am") |> mtof(%) |> map(%, (f) -> osc("tri", f)) |> sum(%) / 3 |> out(%, %)
    )");
    CHECK_FALSE(result.success);
}

TEST_CASE("per-voice filter inside map() with array", "[array][map]") {
    // Uses array literal (not chord pattern) — no poly() needed
    auto result = akkado::compile(
        R"([440, 550, 660] |> map(%, (f) -> osc("saw", f) |> lp(1000, %)) |> sum(%) |> out(%, %))");
    REQUIRE(result.success);

    auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

    int osc_count = 0;
    int lpf_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (insts[i].opcode == cedar::Opcode::OSC_SAW) osc_count++;
        if (insts[i].opcode == cedar::Opcode::FILTER_SVF_LP) lpf_count++;
    }

    CHECK(osc_count == 3);  // 3 oscillators
    CHECK(lpf_count == 3);  // 3 filters (one per voice)
}

TEST_CASE("array literal produces multi-buffer", "[array]") {
    auto result = akkado::compile(
        R"([60, 64, 67] |> map(%, (n) -> mtof(n) |> osc("tri", %)) |> sum(%) |> out(%, %))");
    REQUIRE(result.success);

    auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

    int osc_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (insts[i].opcode == cedar::Opcode::OSC_TRI) {
            osc_count++;
        }
    }

    CHECK(osc_count == 3);  // 3 oscillators for [60, 64, 67]
}

TEST_CASE("chord pattern without poly produces error", "[chord][pattern]") {
    auto result = akkado::compile(
        R"(chord("Am C") |> mtof(%) |> map(%, (f) -> osc("tri", f)) |> sum(%) |> out(%, %))");
    CHECK_FALSE(result.success);
}

// ============================================================================
// Mini-notation chord tests
// ============================================================================

TEST_CASE("chord mini-notation variants produce E410", "[chord][mini]") {
    SECTION("brackets subdivide timing") {
        auto result = akkado::compile("chord(\"[Am C7] Fm Gm\")");
        CHECK_FALSE(result.success);
    }

    SECTION("simple 4-chord pattern") {
        auto result = akkado::compile("chord(\"Am C7 Fm Gm\")");
        CHECK_FALSE(result.success);
    }

    SECTION("repeat modifier") {
        auto result = akkado::compile("chord(\"Am!2 C\")");
        CHECK_FALSE(result.success);
    }

    SECTION("alternating sequence") {
        auto result = akkado::compile("chord(\"<Am C> Fm\")");
        CHECK_FALSE(result.success);
    }

    SECTION("nested brackets") {
        auto result = akkado::compile("chord(\"[[Am C] Dm] Em\")");
        CHECK_FALSE(result.success);
    }

    SECTION("euclidean rhythm") {
        auto result = akkado::compile("chord(\"Am(3,8)\")");
        CHECK_FALSE(result.success);
    }

    SECTION("polyrhythm") {
        auto result = akkado::compile("chord(\"[Am, C, F]\")");
        CHECK_FALSE(result.success);
    }
}

// ============================================================================
// SEQPAT single-voice emission (voice 0 only, polyphony via poly())
// ============================================================================

TEST_CASE("chord emits E410 with voice count info", "[chord][seqpat]") {
    SECTION("triad chord produces E410 with 3 voices") {
        auto result = akkado::compile("chord(\"Am\")");
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E410") {
                found = true;
                CHECK(d.message.find("3 voices") != std::string::npos);
            }
        }
        CHECK(found);
    }

    SECTION("seventh chord produces E410 with 4 voices") {
        auto result = akkado::compile("chord(\"Cmaj7\")");
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E410") {
                found = true;
                CHECK(d.message.find("4 voices") != std::string::npos);
            }
        }
        CHECK(found);
    }
}

TEST_CASE("all chord types produce E410 without poly", "[chord][seqpat]") {
    SECTION("triad") {
        CHECK_FALSE(akkado::compile("chord(\"C\")").success);
    }
    SECTION("seventh chord") {
        CHECK_FALSE(akkado::compile("chord(\"Cmaj7\")").success);
    }
    SECTION("power chord") {
        CHECK_FALSE(akkado::compile("chord(\"C5\")").success);
    }
    SECTION("mixed chord types") {
        CHECK_FALSE(akkado::compile("chord(\"C Cmaj7\")").success);
    }
}

TEST_CASE("pat() with chord symbols produces E410", "[chord][seqpat][pat]") {
    SECTION("C (uppercase chord) without poly is error") {
        auto result = akkado::compile("pat(\"C\")");
        CHECK_FALSE(result.success);
    }

    SECTION("Am7 without poly is error") {
        auto result = akkado::compile("pat(\"Am7\")");
        CHECK_FALSE(result.success);
    }

    SECTION("mixed chords and notes without poly is error") {
        // c4 = single note, C = chord → max_voices > 1
        auto result = akkado::compile("pat(\"c4 C e4\")");
        CHECK_FALSE(result.success);
    }

    SECTION("chord progression in pat() without poly is error") {
        auto result = akkado::compile("pat(\"C F G C\")");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("chord integration with audio graph requires poly()", "[chord][seqpat][integration]") {
    SECTION("chord with osc and out without poly is error") {
        auto result = akkado::compile(
            R"(chord("Am") |> mtof(%) |> map(%, (f) -> osc("sin", f)) |> sum(%) |> out(%, %))");
        CHECK_FALSE(result.success);
    }

    SECTION("seventh chord with filter per voice without poly is error") {
        auto result = akkado::compile(
            R"(chord("Cmaj7") |> mtof(%) |> map(%, (f) -> osc("saw", f) |> lp(2000, %)) |> sum(%) |> out(%, %))");
        CHECK_FALSE(result.success);
    }

    SECTION("pat() chord with processing without poly is error") {
        auto result = akkado::compile(
            R"(pat("C Am") |> map(%, (f) -> osc("tri", f) |> lp(1000, %)) |> sum(%) |> out(%, %))");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("chord accidentals produce E410 without poly", "[chord][seqpat]") {
    CHECK_FALSE(akkado::compile("chord(\"F#m\")").success);
    CHECK_FALSE(akkado::compile("chord(\"Bbmaj7\")").success);
    CHECK_FALSE(akkado::compile("chord(\"Cdim\")").success);
    CHECK_FALSE(akkado::compile("chord(\"Caug\")").success);
    CHECK_FALSE(akkado::compile("chord(\"Csus4 Csus2\")").success);
}

TEST_CASE("chord sequence compilation produces E410", "[chord][seqpat]") {
    CHECK_FALSE(akkado::compile("chord(\"Am C F G\")").success);
    CHECK_FALSE(akkado::compile("chord(\"Am C F\")").success);
    CHECK_FALSE(akkado::compile("chord(\"Am\")").success);
    CHECK_FALSE(akkado::compile("chord(\"[Am C] F\")").success);
}

TEST_CASE("monophonic vs polyphonic pattern detection", "[chord][seqpat]") {
    SECTION("monophonic single notes compile successfully") {
        auto result = akkado::compile("pat(\"c4 e4 g4\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 1);  // Monophonic = single voice
    }

    SECTION("polyphonic chord without poly produces error") {
        auto result = akkado::compile("pat(\"C\")");
        CHECK_FALSE(result.success);
    }

    SECTION("sample patterns compile successfully") {
        auto result = akkado::compile("pat(\"bd sd hh\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 1);  // Sample patterns are monophonic
    }
}

// ============================================================================
// Multi-buffer propagation (arrays still work, chords require poly())
// ============================================================================

TEST_CASE("multi-buffer through variable assignment", "[polyphony][variable]") {
    SECTION("array assigned to variable preserves multi-buffer for map") {
        auto result = akkado::compile(R"(
            x = [440, 550, 660]
            map(x, (f) -> osc("sin", f)) |> sum(%) |> out(%, %)
        )");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int osc_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::OSC_SIN) osc_count++;
        }
        CHECK(osc_count == 3);
    }

    SECTION("chord through variable without poly is error") {
        auto result = akkado::compile(R"(
            ch = chord("Am")
            map(ch, (f) -> osc("sin", f)) |> sum(%) |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("chord field access without poly produces error", "[polyphony][pipe_binding]") {
    SECTION("pat with chord via pipe binding without poly is error") {
        auto result = akkado::compile(R"(
            pat("C") as e |> osc("sin", e.freq) |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }

    SECTION("chord() via pipe binding without poly is error") {
        auto result = akkado::compile(R"(
            chord("Am") as e |> osc("sin", e.freq) |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("pattern field access — monophonic still works", "[polyphony][pattern_var]") {
    SECTION("pattern variable with monophonic pattern accesses .freq") {
        auto result = akkado::compile(R"(
            e = pat("c4 e4 g4")
            osc("sin", e.freq) |> out(%, %)
        )");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int osc_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::OSC_SIN) osc_count++;
        }
        CHECK(osc_count == 1);  // Monophonic = 1 oscillator
    }

    SECTION("pattern variable with chord without poly is error") {
        auto result = akkado::compile(R"(
            e = pat("C")
            osc("sin", e.freq) |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("monophonic pattern field access via pipe", "[polyphony][field_access]") {
    SECTION("monophonic pat() with .freq via % works") {
        auto result = akkado::compile(R"(
            pat("c4 e4 g4") |> osc("sin", %.freq) |> out(%, %)
        )");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int osc_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::OSC_SIN) osc_count++;
        }
        CHECK(osc_count == 1);
    }
}

TEST_CASE("polyphonic field access without poly produces error", "[polyphony][field_access]") {
    SECTION("pat with chord .freq without poly is error") {
        auto result = akkado::compile(R"(
            pat("Am") |> osc("sin", %.freq) |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }

    SECTION("chord() .freq without poly is error") {
        auto result = akkado::compile(R"(
            chord("Am") |> osc("tri", %.freq) |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }

    SECTION("polyphonic .vel without poly is error") {
        auto result = akkado::compile(R"(
            pat("Am") |> osc("sin", %.freq) * %.vel |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }

    SECTION("polyphonic .trig without poly is error") {
        auto result = akkado::compile(R"(
            pat("Am") |> osc("sin", %.freq) * adsr(%.trig, 0.01, 0.1, 0.5, 0.3) |> out(%, %)
        )");
        CHECK_FALSE(result.success);
    }
}
