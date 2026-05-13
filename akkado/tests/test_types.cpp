// Channel-type (Mono / Stereo) semantics for the Akkado compiler.
//
// Covers prd-stereo-support:
//   §4.4 Error cases — left(mono), right(mono), out(stereo, mono), stereo(stereo)
//   §5.3 Type-checking rules (partial: out(L,R) validation, mono() dispatch)
//   §5.2 Auto-lift for stateless opcodes (PRD §5.2 classifies fold/saturate/etc.
//          as auto_lift=true — this used to require stateful ops)
//   §10   Edge cases (mono(mono), stereo(stereo), left/right on mono)
//
// These tests complement test_codegen.cpp's [stereo] tag by focusing on the
// *type* discipline rather than codegen output. If a test fails here, check:
//   - akkado/src/codegen_stereo.cpp  (handle_{stereo,mono,left,right}_call)
//   - akkado/src/codegen.cpp          (out() validation, stereo auto-lift)

#include <catch2/catch_test_macros.hpp>
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
// Auto-lift for stateless opcodes (PRD §5.2 — "Mono-in, mono-out DSP" auto_lift=true)
// =============================================================================

TEST_CASE("Types: stateless opcodes auto-lift on stereo input", "[types][stereo][auto-lift]") {
    // Both saturate (DISTORT_TANH) and softclip (DISTORT_SOFT) are declared
    // with requires_state=false in builtins.hpp — before the G2 fix these
    // stateless ops silently dropped the right channel, contrary to PRD §5.2.
    SECTION("saturate auto-lifts (DISTORT_TANH, stateless)") {
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

    SECTION("softclip auto-lifts (DISTORT_SOFT, stateless)") {
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
// Each auto_lift=true builtin category gets one representative: a stereo input
// must produce a single instruction of the builtin's opcode carrying the
// STEREO_INPUT flag, not two separately-emitted mono passes.
// =============================================================================

TEST_CASE("Types: declarative auto-lift per category", "[types][stereo][auto-lift]") {
    SECTION("filter lp (FILTER_SVF_LP) auto-lifts on stereo") {
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

    SECTION("delay (DELAY) auto-lifts on stereo") {
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

    SECTION("comp (DYNAMICS_COMP) auto-lifts on stereo") {
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

    SECTION("chorus (EFFECT_CHORUS) auto-lifts on stereo") {
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
// Non-auto-lift builtins reject a stereo signal in a Mono slot with E186.
// Special-handler builtins continue to emit E181–E185; E186 is for the generic
// dispatch path only.
// =============================================================================

TEST_CASE("Types: E186 rejects stereo on non-auto-lift builtins", "[types][stereo][errors]") {
    SECTION("adsr with stereo gate is E186") {
        // adsr is intentionally NOT auto-lifted: a stereo gate is a code smell.
        // See plan: `adsr`/`ar` declare their gate slot as Mono, auto_lift=false.
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
        // for non-stereo-native, non-auto-lift builtins like saw().
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

TEST_CASE("Types: mixed mono/stereo arithmetic", "[types][stereo][arithmetic]") {
    // mono + stereo broadcasts the mono operand across both channels.
    // Array-broadcasting in the binary-op path naturally produces 2 output
    // buffers; the current result type is Array(2). A follow-up could tag
    // this explicitly as Stereo for downstream auto-lift, but the audio
    // result is already correct and the test just confirms the program
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
