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

TEST_CASE("chord() integration", "[chord][akkado]") {
    SECTION("single chord produces multi-buffer via SEQPAT") {
        auto result = akkado::compile("chord(\"Am\")");
        REQUIRE(result.success);
        // Am = A, C, E = 3 notes, now uses SEQPAT system
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                seqpat_step_count++;
            }
        }
        CHECK(seqpat_step_count == 3);  // 3 SEQPAT_STEP for Am triad
    }

    SECTION("chord pattern compiles with parallel SEQPAT_STEPs") {
        auto result = akkado::compile("chord(\"Am C7 F G\")");
        REQUIRE(result.success);
        // C7 is a 4-note chord, so max voices = 4
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                seqpat_step_count++;
            }
        }
        CHECK(seqpat_step_count == 4);  // 4 parallel voices (C7 has 4 notes)
    }

    SECTION("chord pattern state init uses SequenceProgram") {
        auto result = akkado::compile("chord(\"Am C F\")");
        REQUIRE(result.success);
        // Should have 1 SequenceProgram state init (shared by all voices)
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);
    }

    SECTION("chord with pipe") {
        auto result = akkado::compile("chord(\"Am\") |> osc(\"saw\", %) |> out(%, %)");
        REQUIRE(result.success);
    }

    SECTION("chord pattern with pipe") {
        auto result = akkado::compile("chord(\"Am C F G\") |> osc(\"saw\", %) |> out(%, %)");
        REQUIRE(result.success);
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

    SECTION("map over chord produces multiple oscillators") {
        auto result = akkado::compile(
            R"(chord("Am") |> mtof(%) |> map(%, (f) -> osc("tri", f)) |> sum(%) |> out(%, %))");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int osc_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::OSC_TRI) {
                osc_count++;
            }
        }
        CHECK(osc_count == 3);  // Am = 3 notes = 3 oscillators
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

    SECTION("sum with map over chord") {
        auto result = akkado::compile(
            R"(chord("C") |> mtof(%) |> map(%, (f) -> osc("sin", f)) |> sum(%) |> out(%, %))");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int add_count = 0;
        int out_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::ADD) add_count++;
            if (insts[i].opcode == cedar::Opcode::OUTPUT) out_count++;
        }

        CHECK(add_count == 2);  // C = 3 notes, 2 ADDs for sum
        CHECK(out_count == 1);  // Single output (summed signal)
    }
}

TEST_CASE("mtof() propagates multi-buffers", "[array][mtof]") {
    SECTION("mtof on chord produces multiple frequencies") {
        auto result = akkado::compile("chord(\"Am\") |> mtof(%)");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int mtof_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::MTOF) {
                mtof_count++;
            }
        }
        CHECK(mtof_count == 3);  // 3 MTOF calls for 3 chord notes
    }
}

TEST_CASE("map() voices have unique state_ids", "[array][map]") {
    auto result = akkado::compile(
        R"(chord("C") |> mtof(%) |> map(%, (f) -> osc("sin", f)) |> sum(%) |> out(%, %))");
    REQUIRE(result.success);

    auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

    std::set<std::uint32_t> state_ids;
    for (std::size_t i = 0; i < count; ++i) {
        if (insts[i].opcode == cedar::Opcode::OSC_SIN) {
            state_ids.insert(insts[i].state_id);
        }
    }

    CHECK(state_ids.size() == 3);  // 3 unique state_ids for C, E, G
}

TEST_CASE("polyphonic chord with averaging", "[chord][polyphony]") {
    // Inline poly pattern: sum(map(c, func)) / len(c)
    // Note: len() only works on array literals, so use constant 3 for Am triad
    auto result = akkado::compile(R"(
        chord("Am") |> mtof(%) |> map(%, (f) -> osc("tri", f)) |> sum(%) / 3 |> out(%, %)
    )");
    REQUIRE(result.success);

    auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

    int osc_count = 0;
    int div_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (insts[i].opcode == cedar::Opcode::OSC_TRI) osc_count++;
        if (insts[i].opcode == cedar::Opcode::DIV) div_count++;
    }

    CHECK(osc_count == 3);  // 3 oscillators for Am triad
    CHECK(div_count == 1);  // 1 division for averaging
}

TEST_CASE("per-voice filter inside map()", "[array][map]") {
    // User explicitly wants per-voice filtering
    auto result = akkado::compile(
        R"(chord("Am") |> mtof(%) |> map(%, (f) -> osc("saw", f) |> lp(1000, %)) |> sum(%) |> out(%, %))");
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

TEST_CASE("chord pattern produces polyphonic sequence", "[chord][pattern]") {
    // Each chord in the pattern should produce multiple voices
    auto result = akkado::compile(
        R"(chord("Am C") |> mtof(%) |> map(%, (f) -> osc("tri", f)) |> sum(%) |> out(%, %))");
    REQUIRE(result.success);

    auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

    int seqpat_step_count = 0;
    int osc_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        if (insts[i].opcode == cedar::Opcode::OSC_TRI) osc_count++;
    }

    // Should have 3 SEQPAT_STEPs (one per voice: root, 3rd, 5th)
    CHECK(seqpat_step_count == 3);
    // Should have 3 oscillators (one per voice)
    CHECK(osc_count == 3);
}

// ============================================================================
// Mini-notation chord tests
// ============================================================================

TEST_CASE("chord with mini-notation brackets", "[chord][mini]") {
    SECTION("brackets subdivide timing - [Am C7] Fm Gm produces 4 chords") {
        auto result = akkado::compile("chord(\"[Am C7] Fm Gm\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init shared by all voices
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);

        // Count SEQPAT_STEP instructions - should be 4 (C7 has 4 notes = max voices)
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        }
        CHECK(seqpat_step_count == 4);  // 4 voices (C7 has 4 notes)
    }

    SECTION("simple 4-chord pattern without brackets") {
        auto result = akkado::compile("chord(\"Am C7 Fm Gm\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);
    }
}

TEST_CASE("chord with repeat modifier", "[chord][mini]") {
    SECTION("Am!2 C repeats Am twice (extends sequence)") {
        // !2 is the repeat modifier - it EXTENDS the sequence (not *2 which compresses)
        // Am!2 C = Am Am C (3 elements, each gets 1/3 of cycle)
        auto result = akkado::compile("chord(\"Am!2 C\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);

        // Count SEQPAT_STEP instructions - should be 3 (triads have 3 notes)
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        }
        CHECK(seqpat_step_count == 3);  // 3 voices (triads)
    }
}

TEST_CASE("chord with alternating sequence", "[chord][mini]") {
    SECTION("<Am C> Fm compiles with SEQPAT") {
        auto result = akkado::compile("chord(\"<Am C> Fm\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);

        // Count SEQPAT_STEP instructions - should be 3 (triads have 3 notes)
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        }
        CHECK(seqpat_step_count == 3);  // 3 voices (triads)
    }
}

TEST_CASE("chord with nested brackets", "[chord][mini]") {
    SECTION("[[Am C] Dm] Em creates nested timing") {
        auto result = akkado::compile("chord(\"[[Am C] Dm] Em\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);

        // Count SEQPAT_STEP instructions - should be 3 (triads have 3 notes)
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        }
        CHECK(seqpat_step_count == 3);  // 3 voices (triads)
    }
}

TEST_CASE("chord with euclidean rhythm", "[chord][mini]") {
    SECTION("Am(3,8) creates euclidean pattern of Am") {
        auto result = akkado::compile("chord(\"Am(3,8)\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);

        // Count SEQPAT_STEP instructions - should be 3 (Am triad = 3 notes)
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        }
        CHECK(seqpat_step_count == 3);  // 3 voices (Am triad)
    }
}

TEST_CASE("chord with polyrhythm", "[chord][mini]") {
    SECTION("[Am, C, F] plays all simultaneously") {
        auto result = akkado::compile("chord(\"[Am, C, F]\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);

        // Count SEQPAT_STEP instructions - should be 3 (triads have 3 notes)
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        }
        CHECK(seqpat_step_count == 3);  // 3 voices (triads)
    }
}

TEST_CASE("chord backward compatibility", "[chord]") {
    SECTION("simple whitespace-separated chords still work") {
        // This is the most common use case - should continue working
        auto result = akkado::compile("chord(\"Am C7 F G\")");
        REQUIRE(result.success);
        // Uses SEQPAT system: 1 SequenceProgram state init
        CHECK(result.state_inits.size() == 1);

        // Count SEQPAT_STEP instructions - should be 4 (C7 has 4 notes = max voices)
        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_step_count++;
        }
        CHECK(seqpat_step_count == 4);  // C7 has 4 notes = 4 voices
    }

    SECTION("single chord still produces multi-buffer via SEQPAT") {
        auto result = akkado::compile("chord(\"Am\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_step_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                seqpat_step_count++;
            }
        }
        CHECK(seqpat_step_count == 3);  // 3 SEQPAT_STEP for Am triad
    }
}

// ============================================================================
// SEQPAT polyphony tests
// ============================================================================

TEST_CASE("SEQPAT voice index parameter", "[chord][seqpat]") {
    SECTION("each voice gets unique voice index") {
        auto result = akkado::compile("chord(\"Am\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        std::vector<std::uint16_t> voice_indices;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                voice_indices.push_back(insts[i].inputs[2]);
            }
        }

        REQUIRE(voice_indices.size() == 3);
        CHECK(voice_indices[0] == 0);  // First voice
        CHECK(voice_indices[1] == 1);  // Second voice
        CHECK(voice_indices[2] == 2);  // Third voice
    }

    SECTION("only first voice outputs velocity and trigger") {
        auto result = akkado::compile("chord(\"Am\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int voice_idx = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                if (voice_idx == 0) {
                    // First voice should have velocity and trigger outputs
                    CHECK(insts[i].inputs[0] != 0xFFFF);  // velocity_buf
                    CHECK(insts[i].inputs[1] != 0xFFFF);  // trigger_buf
                } else {
                    // Secondary voices should NOT output velocity/trigger
                    CHECK(insts[i].inputs[0] == 0xFFFF);
                    CHECK(insts[i].inputs[1] == 0xFFFF);
                }
                voice_idx++;
            }
        }
    }

    SECTION("all voices share same state_id") {
        auto result = akkado::compile("chord(\"Cmaj7\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        std::set<std::uint32_t> state_ids;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) {
                state_ids.insert(insts[i].state_id);
            }
        }

        // All SEQPAT_STEP instructions should share the same state_id
        CHECK(state_ids.size() == 1);
    }
}

TEST_CASE("chord voice count varies by chord type", "[chord][seqpat]") {
    SECTION("triad produces 3 voices") {
        auto result = akkado::compile("chord(\"C\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 3);
    }

    SECTION("seventh chord produces 4 voices") {
        auto result = akkado::compile("chord(\"Cmaj7\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 4);
    }

    SECTION("power chord produces 2 voices") {
        auto result = akkado::compile("chord(\"C5\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 2);
    }

    SECTION("mixed chord types use max voice count") {
        // C (3 notes) + Cmaj7 (4 notes) -> 4 voices total
        auto result = akkado::compile("chord(\"C Cmaj7\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 4);  // Max voices from Cmaj7
    }
}

TEST_CASE("pat() with chord symbols in mini-notation", "[chord][seqpat][pat]") {
    SECTION("C (uppercase) produces 3-voice polyphonic pattern") {
        // In mini-notation, uppercase chord symbols like 'C', 'Am', 'G7' are recognized
        auto result = akkado::compile("pat(\"C\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 3);  // C major triad = 3 notes
    }

    SECTION("Am7 produces 4-voice pattern") {
        auto result = akkado::compile("pat(\"Am7\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 4);  // Am7 = 4 notes
    }

    SECTION("mixed chords and notes in pat()") {
        // c4 = single note (lowercase), C = chord (uppercase)
        auto result = akkado::compile("pat(\"c4 C e4\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        // Max voices determined by C (3 notes)
        CHECK(seqpat_count == 3);
    }

    SECTION("chord progression in pat()") {
        auto result = akkado::compile("pat(\"C F G C\")");
        REQUIRE(result.success);

        // Should use SequenceProgram
        CHECK(result.state_inits.size() == 1);
        CHECK(result.state_inits[0].type == akkado::StateInitData::Type::SequenceProgram);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        int query_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
            if (insts[i].opcode == cedar::Opcode::SEQPAT_QUERY) query_count++;
        }
        CHECK(query_count == 1);   // Single query
        CHECK(seqpat_count == 3);  // 3 voices (all triads)
    }
}

TEST_CASE("SEQPAT chord integration with audio graph", "[chord][seqpat][integration]") {
    SECTION("chord with osc and out") {
        auto result = akkado::compile(
            R"(chord("Am") |> mtof(%) |> map(%, (f) -> osc("sin", f)) |> sum(%) |> out(%, %))");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        int osc_count = 0;
        int mtof_count = 0;
        int add_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
            if (insts[i].opcode == cedar::Opcode::OSC_SIN) osc_count++;
            if (insts[i].opcode == cedar::Opcode::MTOF) mtof_count++;
            if (insts[i].opcode == cedar::Opcode::ADD) add_count++;
        }

        CHECK(seqpat_count == 3);  // 3 voices
        CHECK(mtof_count == 3);    // 3 mtof conversions
        CHECK(osc_count == 3);     // 3 oscillators
        CHECK(add_count == 2);     // sum of 3 = 2 adds
    }

    SECTION("seventh chord with filter per voice") {
        auto result = akkado::compile(
            R"(chord("Cmaj7") |> mtof(%) |> map(%, (f) -> osc("saw", f) |> lp(2000, %)) |> sum(%) |> out(%, %))");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        int osc_count = 0;
        int filter_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
            if (insts[i].opcode == cedar::Opcode::OSC_SAW) osc_count++;
            if (insts[i].opcode == cedar::Opcode::FILTER_SVF_LP) filter_count++;
        }

        CHECK(seqpat_count == 4);  // 4 voices (seventh chord)
        CHECK(osc_count == 4);     // 4 oscillators
        CHECK(filter_count == 4);  // 4 filters
    }

    SECTION("pat() chord with simple processing") {
        // Use uppercase chord symbols in mini-notation
        // Test that each voice gets its own oscillator and filter
        auto result = akkado::compile(
            R"(pat("C Am") |> map(%, (f) -> osc("tri", f) |> lp(1000, %)) |> sum(%) |> out(%, %))");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        int osc_count = 0;
        int filter_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
            if (insts[i].opcode == cedar::Opcode::OSC_TRI) osc_count++;
            if (insts[i].opcode == cedar::Opcode::FILTER_SVF_LP) filter_count++;
        }

        CHECK(seqpat_count == 3);  // 3 voices (triads)
        CHECK(osc_count == 3);
        CHECK(filter_count == 3);
    }
}

TEST_CASE("chord accidentals and inversions", "[chord][seqpat]") {
    SECTION("sharp chord symbols") {
        auto result = akkado::compile("chord(\"F#m\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 3);
    }

    SECTION("flat chord symbols") {
        auto result = akkado::compile("chord(\"Bbmaj7\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 4);  // maj7 = 4 notes
    }

    SECTION("diminished chord") {
        auto result = akkado::compile("chord(\"Cdim\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 3);  // dim = 3 notes
    }

    SECTION("augmented chord") {
        auto result = akkado::compile("chord(\"Caug\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 3);  // aug = 3 notes
    }

    SECTION("suspended chords") {
        auto result = akkado::compile("chord(\"Csus4 Csus2\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 3);  // sus chords = 3 notes
    }
}

TEST_CASE("chord sequence compilation details", "[chord][seqpat]") {
    SECTION("SequenceProgram contains correct event count") {
        auto result = akkado::compile("chord(\"Am C F G\")");
        REQUIRE(result.success);
        REQUIRE(result.state_inits.size() == 1);

        const auto& init = result.state_inits[0];
        CHECK(init.type == akkado::StateInitData::Type::SequenceProgram);
        CHECK(init.total_events >= 4);  // At least 4 chord events
    }

    SECTION("cycle length matches chord count") {
        auto result = akkado::compile("chord(\"Am C F\")");
        REQUIRE(result.success);
        REQUIRE(result.state_inits.size() == 1);

        const auto& init = result.state_inits[0];
        CHECK(init.cycle_length == 3.0f);  // 3 chords = 3 beats
    }

    SECTION("single chord has cycle length 1") {
        auto result = akkado::compile("chord(\"Am\")");
        REQUIRE(result.success);
        REQUIRE(result.state_inits.size() == 1);

        const auto& init = result.state_inits[0];
        CHECK(init.cycle_length == 1.0f);
    }

    SECTION("bracketed chords affect cycle length") {
        // [Am C] F = 2 top-level elements
        auto result = akkado::compile("chord(\"[Am C] F\")");
        REQUIRE(result.success);
        REQUIRE(result.state_inits.size() == 1);

        const auto& init = result.state_inits[0];
        CHECK(init.cycle_length == 2.0f);  // 2 top-level elements
    }
}

TEST_CASE("monophonic vs polyphonic pattern detection", "[chord][seqpat]") {
    SECTION("single notes produce 1 SEQPAT_STEP") {
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

    SECTION("chords produce multiple SEQPAT_STEPs") {
        // Use uppercase chord symbol in mini-notation (not Strudel syntax)
        auto result = akkado::compile("pat(\"C\")");
        REQUIRE(result.success);

        auto insts = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
        std::size_t count = result.bytecode.size() / sizeof(cedar::Instruction);

        int seqpat_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (insts[i].opcode == cedar::Opcode::SEQPAT_STEP) seqpat_count++;
        }
        CHECK(seqpat_count == 3);  // Polyphonic = 3 voices
    }

    SECTION("samples produce 1 SEQPAT_STEP") {
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
