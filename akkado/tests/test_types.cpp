// Channel-type (Mono / Stereo) semantics for the Akkado compiler.
//
// Covers prd-stereo-support:
//   §4.4 Error cases — left(mono), right(mono), out(stereo, mono), stereo(stereo)
//   §5.3 Type-checking rules (partial: out(L,R) validation, mono() dispatch)
//   §5.2 Stereo-native opcodes (fold/saturate/etc. handle both channels in one
//          dispatch; auto-lift retired in prd-stereo-native-opcodes Phase 5)
//   §10   Edge cases (mono(mono), stereo(stereo), left/right on mono)
//
// These tests complement test_codegen.cpp's [stereo] tag by focusing on the
// *type* discipline rather than codegen output. If a test fails here, check:
//   - akkado/src/codegen_stereo.cpp  (handle_{stereo,mono,left,right}_call)
//   - akkado/src/codegen.cpp          (out() validation, stereo-native emit)

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <vector>

static std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> instructions;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    instructions.resize(count);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());
    return instructions;
}

static const cedar::Instruction* find_instruction(const std::vector<cedar::Instruction>& insts,
                                                   cedar::Opcode op) {
    for (const auto& inst : insts) {
        if (inst.opcode == op) return &inst;
    }
    return nullptr;
}

// Deleted rvalue overload: prevents `find_instruction(get_instructions(result), …)`
// from returning a pointer into a destroyed temporary vector. Linux GCC happened
// to leave the freed memory readable; MSVC's debug allocator stomps on it,
// causing flag/rate fields to read garbage.
static const cedar::Instruction* find_instruction(std::vector<cedar::Instruction>&&,
                                                   cedar::Opcode) = delete;

static size_t count_instructions(const std::vector<cedar::Instruction>& insts,
                                  cedar::Opcode op) {
    size_t c = 0;
    for (const auto& inst : insts) if (inst.opcode == op) ++c;
    return c;
}

static bool has_diagnostic(const akkado::CompileResult& result, const std::string& code) {
    for (const auto& d : result.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// =============================================================================
// Channel-mismatch warnings (prd-stereo-native-opcodes §5.6, §9.4)
//
// Pre-PRD, these cases emitted hard errors E181–E185. Post-PRD they auto-
// escalate at the boundary: compilation succeeds, a W18x warning logs the
// redundant/mismatched call at the source location. Type-safety is preserved
// via the still-erroring E186 (non-signal channel mismatch on a non-stereo-
// native builtin).
// =============================================================================

TEST_CASE("Types: left()/right() on mono auto-escalates with warning", "[types][stereo][warnings]") {
    SECTION("left(mono) warns W183 and returns the mono input") {
        auto result = akkado::compile("left(saw(220)) |> out(%)");
        CHECK(result.success);
        CHECK(has_diagnostic(result, "W183"));
    }

    SECTION("right(mono) warns W184 and returns the mono input") {
        auto result = akkado::compile("right(saw(220)) |> out(%)");
        CHECK(result.success);
        CHECK(has_diagnostic(result, "W184"));
    }

    SECTION("left(stereo) still compiles") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            left(s) |> out(%, %)
        )");
        CHECK(result.success);
    }
}

TEST_CASE("Types: stereo() on already-stereo input auto-escalates with warning", "[types][stereo][warnings]") {
    SECTION("stereo(stereo) warns W182 and returns the stereo input") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            stereo(s) |> out(%)
        )");
        CHECK(result.success);
        CHECK(has_diagnostic(result, "W182"));
    }

    SECTION("stereo(mono) still compiles") {
        auto result = akkado::compile("stereo(saw(220)) |> out(%)");
        CHECK(result.success);
    }

    SECTION("stereo(L, R) with two mono signals still compiles") {
        auto result = akkado::compile("stereo(saw(218), saw(222)) |> out(%)");
        CHECK(result.success);
    }
}

TEST_CASE("Types: out(L, R) with mixed channels auto-escalates with warning", "[types][stereo][warnings]") {
    SECTION("out(stereo, mono) warns W185 and routes both to the output bus") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(s, saw(330))
        )");
        CHECK(result.success);
        CHECK(has_diagnostic(result, "W185"));
    }

    SECTION("out(mono, stereo) warns W185 and routes both to the output bus") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(saw(330), s)
        )");
        CHECK(result.success);
        CHECK(has_diagnostic(result, "W185"));
    }

    SECTION("out(mono, mono) still compiles") {
        auto result = akkado::compile("out(saw(218), saw(222))");
        CHECK(result.success);
    }

    SECTION("out(stereo_sig) still compiles (single-arg stereo)") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            out(s)
        )");
        CHECK(result.success);
    }
}

TEST_CASE("Types: mono() on mono auto-escalates with warning", "[types][stereo][warnings]") {
    auto result = akkado::compile("mono(saw(220)) |> out(%)");
    CHECK(result.success);
    CHECK(has_diagnostic(result, "W181"));
}

// =============================================================================
// Stereo-native stateless opcodes (PRD §5.2 — "Mono-in, mono-out DSP")
// =============================================================================

TEST_CASE("Types: stateless opcodes are stereo-native on stereo input", "[types][stereo][stereo-native]") {
    // Both saturate (DISTORT_TANH) and softclip (DISTORT_SOFT) are declared
    // with requires_state=false in builtins.hpp — they still process both
    // channels in a single stereo-native dispatch (STEREO_INPUT flag set).
    SECTION("saturate is stereo-native (DISTORT_TANH, stateless)") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            saturate(s) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::DISTORT_TANH) == 1);
        auto* tanh = find_instruction(insts, cedar::Opcode::DISTORT_TANH);
        REQUIRE(tanh != nullptr);
        CHECK((tanh->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("softclip is stereo-native (DISTORT_SOFT, stateless)") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            softclip(s) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* soft = find_instruction(insts, cedar::Opcode::DISTORT_SOFT);
        REQUIRE(soft != nullptr);
        CHECK((soft->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

// =============================================================================
// Mixed-channel arithmetic (PRD §5.3 rule 4, §10.11)
// =============================================================================

// =============================================================================
// Declarative BuiltinSignature catalog (PRD §5.2, G1)
//
// Each stereo-native builtin category gets one representative: a stereo input
// must produce a single instruction of the builtin's opcode carrying the
// STEREO_INPUT flag (handled in one stereo-native dispatch).
// =============================================================================

TEST_CASE("Types: declarative stereo-native per category", "[types][stereo][stereo-native]") {
    SECTION("filter lp (FILTER_SVF_LP) is stereo-native on stereo") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            lp(s, 800, 0.7) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 1);
        auto* op = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("delay (DELAY) is stereo-native on stereo") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            delay(s, 0.25, 0.5) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        CHECK(count_instructions(insts, cedar::Opcode::DELAY) == 1);
        auto* op = find_instruction(insts, cedar::Opcode::DELAY);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    // freeverb and fdn used to live here as auto-lifted opcodes. They moved
    // to stereo-native in prd-stereo-native-opcodes Phase 2; their tests are
    // in the "Stereo-native opcodes" section below.

    SECTION("comp (DYNAMICS_COMP) is stereo-native on stereo") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            comp(s, -12, 4) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_COMP);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }

    SECTION("chorus (EFFECT_CHORUS) is stereo-native on stereo") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            chorus(s, 0.5, 0.5) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::EFFECT_CHORUS);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

// =============================================================================
// E186 — declarative channel-type mismatch (PRD §5.3 rule 1)
//
// Non-stereo-native builtins reject a stereo signal in a Mono slot with E186.
// Special-handler builtins continue to emit E181–E185; E186 is for the generic
// dispatch path only.
// =============================================================================

TEST_CASE("Types: E186 rejects stereo on non-stereo-native builtins", "[types][stereo][errors]") {
    SECTION("adsr with stereo gate is E186") {
        // adsr is intentionally NOT stereo-native: a stereo gate is a code
        // smell. `adsr`/`ar` declare their gate slot as Mono (PRD §3.2).
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            adsr(s, 0.01, 0.1, 0.5, 0.3) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E186"));
    }

    SECTION("saw oscillator with stereo freq is E186") {
        // Oscillators are mono generators per PRD §5.2; stereo frequency input
        // is rejected rather than silently lifted or chord-expanded.
        auto result = akkado::compile(R"(
            f = stereo(saw(1), saw(2))
            saw(f) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E186"));
    }

    SECTION("mono(mono) emits W181 (not E181 anymore), and never E186") {
        // Special-handler builtins now auto-escalate at the boundary with a
        // W18x warning; the channel-mismatch error path (E186) is reserved
        // for non-stereo-native builtins like saw().
        auto result = akkado::compile("mono(saw(220)) |> out(%)");
        CHECK(result.success);
        CHECK(has_diagnostic(result, "W181"));
        CHECK_FALSE(has_diagnostic(result, "E186"));
    }
}

// =============================================================================
// Stereo-native opcodes (prd-stereo-native-opcodes Phase 0+1)
// =============================================================================

TEST_CASE("Types: stereo-native dattorro produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(110) |> dattorro(%, 0.85, 30) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Exactly one DATTORRO instruction (no auto-lift double emission)
    CHECK(count_instructions(insts, cedar::Opcode::REVERB_DATTORRO) == 1);
    auto* dat = find_instruction(insts, cedar::Opcode::REVERB_DATTORRO);
    REQUIRE(dat != nullptr);
    // STEREO_OUTPUT set, STEREO_INPUT clear (mono primary input)
    CHECK((dat->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((dat->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native dattorro reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(110), saw(111))
        dattorro(s, 0.85, 30) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::REVERB_DATTORRO) == 1);
    auto* dat = find_instruction(insts, cedar::Opcode::REVERB_DATTORRO);
    REQUIRE(dat != nullptr);
    // Both STEREO_OUTPUT and STEREO_INPUT set
    CHECK((dat->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((dat->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native opcode rejects array/chord expansion without poly()", "[types][stereo][stereo-native][errors]") {
    // E187: a multi-voice array (or chord) flowing into a stereo-native
    // opcode requires explicit poly() — they don't silently auto-expand
    // into per-voice instantiation here. Use a 3-element array so the
    // expansion is unambiguously not a stereo pair. The codegen guard is
    // generic across stereo-native opcodes; exercising one per phase is
    // enough to confirm the path stays wired up.
    SECTION("dattorro rejects 3-voice array") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> dattorro(%, 0.85, 30) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }

    SECTION("freeverb rejects 3-voice array") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> freeverb(%, 0.85, 0.5) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }

    SECTION("fdn rejects 3-voice array") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> fdn(%, 0.85, 0.3) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }
}

// =============================================================================
// Stereo-native reverbs (prd-stereo-native-opcodes Phase 2)
// =============================================================================

TEST_CASE("Types: stereo-native freeverb produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(110) |> freeverb(%, 0.85, 0.5) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::REVERB_FREEVERB) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::REVERB_FREEVERB);
    REQUIRE(op != nullptr);
    // STEREO_OUTPUT set, STEREO_INPUT clear (mono primary input).
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native freeverb reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        freeverb(s, 0.85, 0.5) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    // Exactly one instruction: no auto-lift double-emission.
    CHECK(count_instructions(insts, cedar::Opcode::REVERB_FREEVERB) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::REVERB_FREEVERB);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native fdn produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(110) |> fdn(%, 0.85, 0.3) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::REVERB_FDN) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::REVERB_FDN);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native fdn reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        fdn(s, 0.85, 0.3) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::REVERB_FDN) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::REVERB_FDN);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

// =============================================================================
// Stereo-native modulation FX + sampler (prd-stereo-native-opcodes Phase 3)
// =============================================================================

TEST_CASE("Types: stereo-native chorus produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> chorus(%, 0.5, 0.4) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::EFFECT_CHORUS) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_CHORUS);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native chorus reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        chorus(s, 0.5, 0.4) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::EFFECT_CHORUS) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_CHORUS);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native flanger produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> flanger(%, 1.0, 0.7) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::EFFECT_FLANGER) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_FLANGER);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native flanger reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        flanger(s, 1.0, 0.7) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::EFFECT_FLANGER) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_FLANGER);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native phaser produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> phaser(%, 0.5, 0.8) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::EFFECT_PHASER) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_PHASER);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native phaser reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        phaser(s, 0.5, 0.8) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::EFFECT_PHASER) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_PHASER);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native modulation FX reject chord/array expansion", "[types][stereo][stereo-native][errors]") {
    SECTION("chorus rejects 3-voice array") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> chorus(%, 0.5, 0.4) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }
    SECTION("flanger rejects 3-voice array") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> flanger(%, 1.0, 0.7) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }
    SECTION("phaser rejects 3-voice array") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> phaser(%, 0.5, 0.8) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }
}

TEST_CASE("Types: stereo-native sample emits SAMPLE_PLAY with STEREO_OUTPUT flag", "[types][stereo][stereo-native]") {
    // Scalar sample() call. The 3rd arg "bd" resolves to a sample name; we
    // only care that SAMPLE_PLAY is emitted with the correct flags.
    auto result = akkado::compile(R"(
        sample(1.0, 1.0, "bd") |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::SAMPLE_PLAY) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::SAMPLE_PLAY);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

TEST_CASE("Types: stereo-native sample_loop emits SAMPLE_PLAY_LOOP with STEREO_OUTPUT flag", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        sample_loop(1.0, 1.0, "bd") |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::SAMPLE_PLAY_LOOP) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::SAMPLE_PLAY_LOOP);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

// Step D — re-validation of pre-existing stereo helpers (pan/width/ms/pingpong)
// in chain with the newly stereo-native Phase 3 opcodes. These helpers use a
// different codegen path (handle_*_call dispatch + register_stereo memory
// tracking) than the stereo_native flag; this test simply confirms that the
// two mechanisms compose cleanly.
TEST_CASE("Types: stereo-native FX chain through existing stereo helpers", "[types][stereo][stereo-native]") {
    SECTION("chorus → width") {
        auto result = akkado::compile(R"(
            saw(220) |> chorus(%, 0.5, 0.4) |> width(%, 1.5) |> out(%)
        )");
        CHECK(result.success);
    }
    SECTION("flanger → pan") {
        auto result = akkado::compile(R"(
            saw(220) |> flanger(%, 1.0, 0.7) |> pan(%, 0.3) |> out(%)
        )");
        CHECK(result.success);
    }
    SECTION("phaser → pingpong") {
        auto result = akkado::compile(R"(
            saw(220) |> phaser(%, 0.5, 0.8) |> pingpong(%, 0.25, 0.5) |> out(%)
        )");
        CHECK(result.success);
    }
    SECTION("chorus → ms_encode → ms_decode round-trip") {
        auto result = akkado::compile(R"(
            saw(220) |> chorus(%, 0.5, 0.4) |> ms_encode(%) |> ms_decode(%) |> out(%)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Stereo-native filters (prd-stereo-native-opcodes Phase 4a)
// =============================================================================

TEST_CASE("Types: stereo-native lp produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> lp(%, 800, 0.7) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native lp reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        lp(s, 800, 0.7) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_SVF_LP);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native hp produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> hp(%, 800, 0.7) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_SVF_HP);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

TEST_CASE("Types: stereo-native bp produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> bp(%, 800, 2.0) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_SVF_BP);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

TEST_CASE("Types: stereo-native moog produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> moog(%, 1000, 1.5) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_MOOG);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native moog reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        moog(s, 1000, 1.5) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_MOOG);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native diode produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> diode(%, 800, 2.0) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_DIODE);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

TEST_CASE("Types: stereo-native formant produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> formant(%, 0.0, 1.0, 0.5, 10.0) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_FORMANT);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

TEST_CASE("Types: stereo-native sallenkey produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> sallenkey(%, 800, 1.5, 0.0) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::FILTER_SALLENKEY);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

TEST_CASE("Types: stereo-native filters reject chord/array on signal slot", "[types][stereo][stereo-native][errors]") {
    SECTION("lp rejects 3-voice chord on signal slot") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> lp(%, 800) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }
    SECTION("moog rejects 3-voice chord on signal slot") {
        auto result = akkado::compile(R"(
            voices = [saw(110), saw(220), saw(330)]
            voices |> moog(%, 1000, 1.5) |> out(%)
        )");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic(result, "E187"));
    }
}

TEST_CASE("Types: stereo-native filters accept array on control slot (UGen expansion)", "[types][stereo][stereo-native]") {
    // Array expansion on a non-signal slot (cutoff here) is UGen auto-expansion,
    // not chord expansion — emits N stereo-native filter instances. See
    // prd-stereo-native-opcodes §9.10.
    auto result = akkado::compile(R"(
        ns = noise()
        freqs = [500, 1000, 2000]
        freqs |> lp(ns, %)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::FILTER_SVF_LP) == 3);
    // Each emitted instance carries STEREO_OUTPUT.
    int stereo_out_count = 0;
    for (const auto& inst : insts) {
        if (inst.opcode == cedar::Opcode::FILTER_SVF_LP &&
            (inst.flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0) {
            ++stereo_out_count;
        }
    }
    CHECK(stereo_out_count == 3);
}

TEST_CASE("Types: stereo-native filter chain through existing stereo helpers", "[types][stereo][stereo-native]") {
    SECTION("lp → width") {
        auto result = akkado::compile(R"(
            saw(220) |> lp(%, 800) |> width(%, 1.5) |> out(%)
        )");
        CHECK(result.success);
    }
    SECTION("moog → pan") {
        auto result = akkado::compile(R"(
            saw(220) |> moog(%, 1000, 1.5) |> pan(%, 0.3) |> out(%)
        )");
        CHECK(result.success);
    }
    SECTION("formant → ms_encode → ms_decode") {
        auto result = akkado::compile(R"(
            saw(220) |> formant(%, 0.0, 1.0, 0.5, 10.0) |> ms_encode(%) |> ms_decode(%) |> out(%)
        )");
        CHECK(result.success);
    }
}

// =============================================================================
// Stereo-native distortion (prd-stereo-native-opcodes Phase 4b)
// =============================================================================

TEST_CASE("Types: stereo-native saturate produces stereo output", "[types][stereo][stereo-native]") {
    SECTION("mono in") {
        auto result = akkado::compile(R"( saw(220) |> saturate(%, 2.0) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_TANH);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
    }
    SECTION("stereo in") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            saturate(s, 2.0) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_TANH);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

TEST_CASE("Types: stereo-native softclip/fold/bitcrush produce stereo output", "[types][stereo][stereo-native]") {
    SECTION("softclip mono in") {
        auto result = akkado::compile(R"( saw(220) |> softclip(%, 0.5) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_SOFT);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("fold mono in") {
        auto result = akkado::compile(R"( saw(220) |> fold(%, 0.5) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_FOLD);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("bitcrush mono in") {
        auto result = akkado::compile(R"( saw(220) |> bitcrush(%, 8, 0.5) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_BITCRUSH);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
}

TEST_CASE("Types: stereo-native oversampled distortion (tube/smooth/tape/xfmr/excite)", "[types][stereo][stereo-native]") {
    SECTION("tube") {
        auto result = akkado::compile(R"( saw(220) |> tube(%, 5.0, 0.1) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_TUBE);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("smooth") {
        auto result = akkado::compile(R"( saw(220) |> smooth(%, 5.0) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_SMOOTH);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("tape") {
        auto result = akkado::compile(R"( saw(220) |> tape(%, 3.0, 0.3) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_TAPE);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("xfmr") {
        auto result = akkado::compile(R"( saw(220) |> xfmr(%, 3.0, 5.0) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_XFMR);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("excite") {
        auto result = akkado::compile(R"( saw(220) |> excite(%, 0.5, 3000) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DISTORT_EXCITE);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
}

TEST_CASE("Types: stereo-native filter+distortion chain", "[types][stereo][stereo-native]") {
    // End-to-end: stereo-native chains through filters + distortion + helpers.
    auto result = akkado::compile(R"(
        saw(220) |> lp(%, 1000) |> saturate(%, 2.0) |> width(%, 1.4) |> out(%)
    )");
    CHECK(result.success);
}

// =============================================================================
// Stereo-native delays + comb (prd-stereo-native-opcodes Phase 4c)
// =============================================================================

TEST_CASE("Types: stereo-native delay produces stereo output from mono input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"( saw(220) |> delay(%, 0.25, 0.5) |> out(%) )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::DELAY);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native delay reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        delay(s, 0.25, 0.5) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::DELAY);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: stereo-native delay_ms and delay_smp inherit inst_rate", "[types][stereo][stereo-native]") {
    SECTION("delay_ms encodes rate=1") {
        auto result = akkado::compile(R"( saw(220) |> delay_ms(%, 250, 0.5) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DELAY);
        REQUIRE(op != nullptr);
        CHECK(op->rate == 1);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("delay_smp encodes rate=2") {
        auto result = akkado::compile(R"( saw(220) |> delay_smp(%, 12000, 0.5) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DELAY);
        REQUIRE(op != nullptr);
        CHECK(op->rate == 2);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
}

TEST_CASE("Types: stereo-native comb produces stereo output", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"( saw(220) |> comb(%, 5, 0.7) |> out(%) )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_COMB);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
}

// =============================================================================
// Stereo-native dynamics + env_follower (prd-stereo-native-opcodes Phase 4d)
// =============================================================================

TEST_CASE("Types: stereo-native comp produces stereo output", "[types][stereo][stereo-native]") {
    SECTION("mono in") {
        auto result = akkado::compile(R"( saw(220) |> comp(%, -12, 4) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_COMP);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
    }
    SECTION("stereo in") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            comp(s, -12, 4) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_COMP);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

TEST_CASE("Types: stereo-native limiter and gate produce stereo output", "[types][stereo][stereo-native]") {
    SECTION("limiter") {
        auto result = akkado::compile(R"( saw(220) |> limiter(%, -0.1, 0.1) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_LIMITER);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("gate") {
        auto result = akkado::compile(R"( saw(220) |> gate(%, -40, -40) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_GATE);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
}

TEST_CASE("Types: env_follower output width matches input", "[types][stereo][stereo-native]") {
    SECTION("mono in produces mono envelope (no STEREO_OUTPUT)") {
        auto result = akkado::compile(R"( saw(220) |> env_follower(%, 0.01, 0.1) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::ENV_FOLLOWER);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) == 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
    }
    SECTION("stereo in produces per-channel envelopes") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            env_follower(s, 0.01, 0.1) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::ENV_FOLLOWER);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

TEST_CASE("Types: full Phase 4 stereo chain — filter+distortion+comp+limiter", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        saw(220) |> lp(%, 1000) |> saturate(%, 2.0) |> comp(%, -12, 4)
                 |> limiter(%, -0.1, 0.1) |> out(%)
    )");
    CHECK(result.success);
}

TEST_CASE("Types: stereo-native tap_delay runs closure on stereo signal pair", "[types][stereo][stereo-native]") {
    // tap_delay's closure body operates on a stereo signal pair (not per-
    // channel closure execution). Inner stereo-native ops (lp/saturate)
    // consume the stereo pair and produce a stereo pair.
    SECTION("tap_delay with saturate closure") {
        auto result = akkado::compile(R"(
            saw(220) |> tap_delay(%, 0.25, 0.5, (x) -> saturate(x, 2.0)) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* tap = find_instruction(insts, cedar::Opcode::DELAY_TAP);
        auto* write = find_instruction(insts, cedar::Opcode::DELAY_WRITE);
        REQUIRE(tap != nullptr);
        REQUIRE(write != nullptr);
        CHECK((tap->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((write->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    }
    SECTION("tap_delay with lp closure") {
        auto result = akkado::compile(R"(
            saw(220) |> tap_delay(%, 0.25, 0.5, (x) -> lp(x, 800)) |> out(%)
        )");
        CHECK(result.success);
    }
    SECTION("tap_delay_ms passes time unit through to TAP rate field") {
        auto result = akkado::compile(R"(
            saw(220) |> tap_delay_ms(%, 250, 0.5, (x) -> lp(x, 800)) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* tap = find_instruction(insts, cedar::Opcode::DELAY_TAP);
        REQUIRE(tap != nullptr);
        CHECK(tap->rate == 1);
    }
}

// =============================================================================
// Stereo-native control primitives (prd-stereo-native-opcodes Phase 5)
// slew + the EDGE_OP family (sah/gateup/gatedown/counter) — the last auto_lift
// holdouts, now stereo-native. Auto-lift is fully retired.
// =============================================================================

TEST_CASE("Types: slew with mono input stays mono", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"( saw(220) |> slew(%, 10.0) |> out(%) )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::SLEW) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::SLEW);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) == 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

TEST_CASE("Types: stereo-native slew reads stereo primary input", "[types][stereo][stereo-native]") {
    auto result = akkado::compile(R"(
        s = stereo(saw(218), saw(222))
        slew(s, 10.0) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::SLEW) == 1);
    auto* op = find_instruction(insts, cedar::Opcode::SLEW);
    REQUIRE(op != nullptr);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
    CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
}

TEST_CASE("Types: EDGE_OP family output width matches input", "[types][stereo][stereo-native]") {
    // All four edge primitives share Opcode::EDGE_OP, mode-dispatched on rate.
    SECTION("sah — mono in, mono out") {
        auto result = akkado::compile(R"( saw(220) |> sah(%, beat(1)) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::EDGE_OP);
        REQUIRE(op != nullptr);
        CHECK(op->rate == 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) == 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
    }
    SECTION("gateup — mono in, mono out") {
        auto result = akkado::compile(R"( saw(220) |> gateup(%) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::EDGE_OP);
        REQUIRE(op != nullptr);
        CHECK(op->rate == 1);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) == 0);
    }
    SECTION("gatedown — mono in, mono out") {
        auto result = akkado::compile(R"( saw(220) |> gatedown(%) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::EDGE_OP);
        REQUIRE(op != nullptr);
        CHECK(op->rate == 2);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) == 0);
    }
    SECTION("counter — mono in, mono out") {
        auto result = akkado::compile(R"( beat(1) |> counter(%) |> out(%) )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::EDGE_OP);
        REQUIRE(op != nullptr);
        CHECK(op->rate == 3);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) == 0);
    }
    SECTION("sah reads stereo primary input") {
        auto result = akkado::compile(R"(
            s = stereo(saw(218), saw(222))
            sah(s, beat(1)) |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        auto* op = find_instruction(insts, cedar::Opcode::EDGE_OP);
        REQUIRE(op != nullptr);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_OUTPUT) != 0);
        CHECK((op->flags & cedar::InstructionFlag::STEREO_INPUT) != 0);
    }
}

TEST_CASE("Types: glide on mono pattern field feeds mono saw freq slot",
          "[types][stereo][stereo-native]") {
    // Canonical example from prd-glide-interp.md §3.1 — used to fail with
    // E186 because glide() forced Stereo output into saw()'s mono freq slot.
    // ChannelCount::Match keeps the smoothed control signal mono.
    auto result = akkado::compile(R"( n"c4 c5" |> saw(glide(@.freq, 0.1)) |> out(@) )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    CHECK(count_instructions(insts, cedar::Opcode::INTERP_TIME) == 1);
    auto* interp = find_instruction(insts, cedar::Opcode::INTERP_TIME);
    REQUIRE(interp != nullptr);
    CHECK((interp->flags & cedar::InstructionFlag::STEREO_OUTPUT) == 0);
    CHECK((interp->flags & cedar::InstructionFlag::STEREO_INPUT) == 0);
}

// ============================================================================
// Extended params (prd-extended-params §5–§6) — canonical mechanism for
// opcode parameters beyond the 5 input-buffer slots.
// ============================================================================

static const akkado::StateInitData* find_ext_params_init(const akkado::CompileResult& r) {
    for (const auto& s : r.state_inits) {
        if (s.type == akkado::StateInitData::Type::ExtendedParams) return &s;
    }
    return nullptr;
}

TEST_CASE("ExtendedParams: chorus emits StateInit with default lfo_phase=0.25",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> chorus(%, 0.5, 0.4) |> out(%)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    // Post-unified-drywet: chorus extended slots are [lfo_phase, dry, wet]
    CHECK(ext->ext_count == 3);
    CHECK(ext->ext_buffer_indices[0] == 0xFFFFu);
    CHECK(ext->ext_constants[0] == Catch::Approx(0.25f));
    CHECK(ext->ext_constants[1] == Catch::Approx(1.0f));  // dry default (Cat A)
    CHECK(ext->ext_constants[2] == Catch::Approx(0.5f));  // wet default (Cat A)
}

TEST_CASE("ExtendedParams: chorus accepts named lfo_phase literal",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> chorus(%, 0.5, 0.4, lfo_phase: 0.5) |> out(%)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 3);
    // Literal lowers via PUSH_CONST + buffer — buffer_idx != 0xFFFF.
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);
}

TEST_CASE("ExtendedParams: flanger default feedback/lfo_phase/dry/wet",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> flanger(%, 1.0, 0.5) |> out(%)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    // Post-migration: flanger extended slots are [feedback, lfo_phase, dry, wet]
    CHECK(ext->ext_count == 4);
    CHECK(ext->ext_constants[0] == Catch::Approx(-0.99f));  // feedback (rate=0 decode)
    CHECK(ext->ext_constants[1] == Catch::Approx(0.25f));   // lfo_phase
    CHECK(ext->ext_constants[2] == Catch::Approx(1.0f));    // dry (Cat A)
    CHECK(ext->ext_constants[3] == Catch::Approx(0.5f));    // wet (Cat A)
}

TEST_CASE("ExtendedParams: flanger accepts named feedback, drops rate-field packing",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> flanger(%, 1.0, 0.5, feedback: 0.6) |> out(%)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 4);
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);  // feedback provided
    CHECK(ext->ext_buffer_indices[1] == 0xFFFFu);  // lfo_phase default
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_FLANGER);
    REQUIRE(op != nullptr);
    CHECK(op->rate == 0u);
}

TEST_CASE("ExtendedParams: phaser emits 5-slot StateInit with defaults",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> phaser(%, 0.5, 0.8) |> out(%)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    // Post-unified-drywet: phaser extended slots are
    // [feedback, stages, lfo_phase, dry, wet]
    CHECK(ext->ext_count == 5);
    CHECK(ext->ext_constants[0] == Catch::Approx(0.5f));
    CHECK(ext->ext_constants[1] == Catch::Approx(4.0f));
    CHECK(ext->ext_constants[2] == Catch::Approx(0.25f));
    CHECK(ext->ext_constants[3] == Catch::Approx(1.0f));  // dry (Cat A)
    CHECK(ext->ext_constants[4] == Catch::Approx(0.5f));  // wet (Cat A)
    CHECK(ext->ext_buffer_indices[0] == 0xFFFFu);
    CHECK(ext->ext_buffer_indices[1] == 0xFFFFu);
    CHECK(ext->ext_buffer_indices[2] == 0xFFFFu);
    CHECK(ext->ext_buffer_indices[3] == 0xFFFFu);
    CHECK(ext->ext_buffer_indices[4] == 0xFFFFu);
}

TEST_CASE("ExtendedParams: phaser feedback/stages/lfo_phase all customizable by name",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> phaser(%, 0.5, 0.8, feedback: 0.7, stages: 6, lfo_phase: 0.4) |> out(%)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 5);
    // feedback/stages/lfo_phase are all buffer-backed; dry/wet take defaults.
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);
    CHECK(ext->ext_buffer_indices[1] != 0xFFFFu);
    CHECK(ext->ext_buffer_indices[2] != 0xFFFFu);
}

TEST_CASE("ExtendedParams: phaser drops rate-field bit-packing entirely",
          "[types][stereo-native][extended-params]") {
    // Phaser used to pack feedback/stages into inst.rate. After the
    // ExtendedParams migration the rate field must be zero — the param
    // values now live in the ExtendedParams<3> state init.
    auto result = akkado::compile(R"(
        saw(220) |> phaser(%, 0.5, 0.8, feedback: 0.7, stages: 6) |> out(%)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_PHASER);
    REQUIRE(op != nullptr);
    CHECK(op->rate == 0u);
}

TEST_CASE("ExtendedParams: skipping intermediate ext params keeps named arg aligned",
          "[types][stereo-native][extended-params]") {
    // `phaser(..., lfo_phase: 0.3)` skips feedback/stages — the analyzer's
    // gap-fill inserts placeholders so lfo_phase still lands in ext slot 2,
    // and feedback/stages take their defaults.
    auto result = akkado::compile(R"(
        saw(220) |> phaser(%, 0.5, 0.8, lfo_phase: 0.3) |> out(%)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 5);
    // ext[0]=feedback, ext[1]=stages get defaults via the `_` placeholder
    // PUSH_CONST path so they're buffer-backed with the default value.
    // ext[2]=lfo_phase is also buffer-backed from the user's 0.3 literal.
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);
    CHECK(ext->ext_buffer_indices[1] != 0xFFFFu);
    CHECK(ext->ext_buffer_indices[2] != 0xFFFFu);
    // ext[3]=dry, ext[4]=wet remain as constants (not specified by user).
    CHECK(ext->ext_buffer_indices[3] == 0xFFFFu);
    CHECK(ext->ext_buffer_indices[4] == 0xFFFFu);
}

// --- prd-extended-params-migration: dynamics (comp, limiter, gate) ----------

TEST_CASE("ExtendedParams: comp emits 2-slot StateInit with attack/release defaults",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> comp(@, -12, 4) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 2);
    CHECK(ext->ext_constants[0] == Catch::Approx(0.1f));   // attack ms
    CHECK(ext->ext_constants[1] == Catch::Approx(10.0f));  // release ms
    CHECK(ext->ext_buffer_indices[0] == 0xFFFFu);
    CHECK(ext->ext_buffer_indices[1] == 0xFFFFu);
}

TEST_CASE("ExtendedParams: comp accepts named attack/release literals",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> comp(@, -12, 4, attack: 5, release: 200) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 2);
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);
    CHECK(ext->ext_buffer_indices[1] != 0xFFFFu);
}

TEST_CASE("ExtendedParams: comp drops rate-field bit-packing entirely",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> comp(@, -12, 4, attack: 5) |> out(@)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_COMP);
    REQUIRE(op != nullptr);
    CHECK(op->rate == 0u);
}

TEST_CASE("ExtendedParams: limiter emits 1-slot StateInit with lookahead=0 default",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> limiter(@, -0.1, 100) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 1);
    CHECK(ext->ext_constants[0] == Catch::Approx(0.0f));  // lookahead off
    CHECK(ext->ext_buffer_indices[0] == 0xFFFFu);
}

TEST_CASE("ExtendedParams: limiter drops rate-field toggle entirely",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> limiter(@, -0.1, 100, lookahead: 1) |> out(@)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_LIMITER);
    REQUIRE(op != nullptr);
    CHECK(op->rate == 0u);
}

TEST_CASE("ExtendedParams: gate emits 5-slot StateInit (attack/hold/release/dry/wet)",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> gate(@, -40, -40) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 5);
    CHECK(ext->ext_constants[0] == Catch::Approx(0.1f));   // attack ms
    CHECK(ext->ext_constants[1] == Catch::Approx(0.0f));   // hold ms
    CHECK(ext->ext_constants[2] == Catch::Approx(10.0f));  // release ms
    CHECK(ext->ext_constants[3] == Catch::Approx(0.0f));   // dry (Cat B)
    CHECK(ext->ext_constants[4] == Catch::Approx(1.0f));   // wet (Cat B)
}

TEST_CASE("ExtendedParams: gate keeps dry/wet aligned when attack/release named",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> gate(@, -40, -40, attack: 2, release: 100) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 5);
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);  // attack provided
    CHECK(ext->ext_buffer_indices[2] != 0xFFFFu);  // release provided
    CHECK(ext->ext_buffer_indices[3] == 0xFFFFu);  // dry default
    CHECK(ext->ext_buffer_indices[4] == 0xFFFFu);  // wet default
}

TEST_CASE("ExtendedParams: gate drops rate-field bit-packing entirely",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> gate(@, -40, -40, hold: 50) |> out(@)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::DYNAMICS_GATE);
    REQUIRE(op != nullptr);
    CHECK(op->rate == 0u);
}

// --- prd-extended-params-migration: reverbs (dattorro) ----------------------

TEST_CASE("ExtendedParams: dattorro emits 5-slot StateInit (damping/mod_depth/lfo_rate/dry/wet)",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> dattorro(@, 0.7, 20) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 5);
    CHECK(ext->ext_constants[0] == Catch::Approx(0.0f));   // damping
    CHECK(ext->ext_constants[1] == Catch::Approx(0.0f));   // mod_depth
    CHECK(ext->ext_constants[2] == Catch::Approx(0.5f));   // lfo_rate Hz
    CHECK(ext->ext_constants[3] == Catch::Approx(1.0f));   // dry (Cat A)
    CHECK(ext->ext_constants[4] == Catch::Approx(0.5f));   // wet (Cat A)
}

TEST_CASE("ExtendedParams: dattorro accepts named damping/mod_depth/lfo_rate",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> dattorro(@, 0.7, 20, damping: 0.4, mod_depth: 0.3, lfo_rate: 0.8) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 5);
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);  // damping provided
    CHECK(ext->ext_buffer_indices[1] != 0xFFFFu);  // mod_depth provided
    CHECK(ext->ext_buffer_indices[2] != 0xFFFFu);  // lfo_rate provided
    CHECK(ext->ext_buffer_indices[3] == 0xFFFFu);  // dry default
    CHECK(ext->ext_buffer_indices[4] == 0xFFFFu);  // wet default
}

TEST_CASE("ExtendedParams: dattorro drops rate-field bit-packing entirely",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> dattorro(@, 0.7, 20, damping: 0.5) |> out(@)
    )");
    REQUIRE(result.success);
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::REVERB_DATTORRO);
    REQUIRE(op != nullptr);
    CHECK(op->rate == 0u);
}

// NOTE: seqpat_transport's cycle_length migration (inputs[3]/[4] halving →
// ExtendedParams<1>) has no Akkado [extended-params] test because transport()
// is not registered as a builtin — it is unreachable from source today (the
// codegen handler and VM dispatch shipped in 5cb4eda but the analyzer-facing
// builtin entry was never added). The codegen + opcode-body changes stay
// consistent so the path is correct once transport() is wired up.

// --- prd-extended-params-migration: modulation (comb) -----------------------

TEST_CASE("ExtendedParams: comb emits 1-slot StateInit with damping=0 default",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> comb(@, 5, 0.9) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 1);
    CHECK(ext->ext_constants[0] == Catch::Approx(0.0f));  // damping
    CHECK(ext->ext_buffer_indices[0] == 0xFFFFu);
}

TEST_CASE("ExtendedParams: comb accepts named damping, drops rate-field packing",
          "[types][stereo-native][extended-params]") {
    auto result = akkado::compile(R"(
        saw(220) |> comb(@, 5, 0.9, damping: 0.4) |> out(@)
    )");
    REQUIRE(result.success);
    const auto* ext = find_ext_params_init(result);
    REQUIRE(ext != nullptr);
    CHECK(ext->ext_count == 1);
    CHECK(ext->ext_buffer_indices[0] != 0xFFFFu);  // damping provided
    auto insts = get_instructions(result);
    auto* op = find_instruction(insts, cedar::Opcode::EFFECT_COMB);
    REQUIRE(op != nullptr);
    CHECK(op->rate == 0u);
}

TEST_CASE("Types: mixed mono/stereo arithmetic", "[types][stereo][arithmetic]") {
    // mono + stereo broadcasts the mono operand across both channels.
    // Array-broadcasting in the binary-op path naturally produces 2 output
    // buffers; the current result type is Array(2). A follow-up could tag
    // this explicitly as Stereo for downstream stereo-native ops, but the
    // audio result is already correct and the test just confirms the program
    // compiles and emits one ADD per channel.
    SECTION("mono * stereo compiles and produces 2 multiplies") {
        auto result = akkado::compile(R"(
            dry = saw(220)
            wet = stereo(saw(218), saw(222))
            dry * 0.3 + wet * 0.7 |> out(%)
        )");
        REQUIRE(result.success);
        auto insts = get_instructions(result);
        // One MUL for dry * 0.3 (mono), two for wet * 0.7 (stereo broadcast),
        // so at least 3 multiplies total.
        CHECK(count_instructions(insts, cedar::Opcode::MUL) >= 3);
    }
}
