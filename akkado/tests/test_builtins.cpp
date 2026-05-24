#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include "akkado/builtins.hpp"
#include "akkado/stdlib.hpp"

using namespace akkado;

TEST_CASE("fmod builtin is registered", "[builtins][fmod]") {
    const auto* fmod_b = lookup_builtin("fmod");
    REQUIRE(fmod_b != nullptr);
    CHECK(fmod_b->total_params() == 2);
    CHECK(fmod_b->find_param("a") == 0);
    CHECK(fmod_b->find_param("b") == 1);
    CHECK(fmod_b->requires_state == false);
}

TEST_CASE("stdlib embed mechanism wires event_transforms.ak", "[stdlib][embed]") {
    // The CMake step embeds every `akkado/stdlib/*.ak` into STDLIB_EMBEDDED_FILES.
    // Phase 2b ships event_transforms.ak; confirm it's reachable from C++.
    bool found_event_transforms = false;
    for (const auto& f : STDLIB_EMBEDDED_FILES) {
        if (f.name == "<stdlib/event_transforms.ak>") {
            found_event_transforms = true;
            CHECK_FALSE(f.source.empty());
        }
    }
    CHECK(found_event_transforms);
}

TEST_CASE("stdlib embed mechanism wires scales.ak (Phase 5 Commit B)",
          "[stdlib][embed][scale][stdlib-wiring]") {
    // Phase 5 Commit B: scales.ak is the placeholder for the dispatcher
    // catalog landing in Commit C. Confirm the new file is glob-picked up
    // and reachable as STDLIB_EMBEDDED_FILES content.
    bool found_scales = false;
    for (const auto& f : STDLIB_EMBEDDED_FILES) {
        if (f.name == "<stdlib/scales.ak>") {
            found_scales = true;
            CHECK_FALSE(f.source.empty());
        }
    }
    CHECK(found_scales);
}

TEST_CASE("scales.ak fn resolves through the loader (Phase 5 Commit B)",
          "[scale][stdlib-wiring]") {
    // End-to-end: load + parse + analyze + reference a fn defined in
    // scales.ak. If anything in the loader→parser→analyzer chain rejected
    // the new stdlib file this fails to compile.
    auto r = akkado::compile("scales_smoke()");
    REQUIRE(r.success);
}

TEST_CASE("BuiltinInfo methods", "[builtins]") {
    SECTION("total_params for saw") {
        const auto& saw = BUILTIN_FUNCTIONS.at("saw");
        // saw has 1 required + 2 optional = 3
        CHECK(saw.total_params() == 3);
    }

    SECTION("total_params for lp filter") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        // lp has 2 required + 3 optional (q, dry, wet) = 5
        CHECK(lp.total_params() == 5);
    }

    SECTION("total_params for stateless functions") {
        const auto& add = BUILTIN_FUNCTIONS.at("add");
        // add has 2 required + 0 optional = 2
        CHECK(add.total_params() == 2);
    }

    SECTION("find_param by name - saw") {
        const auto& saw = BUILTIN_FUNCTIONS.at("saw");
        CHECK(saw.find_param("freq") == 0);
        CHECK(saw.find_param("phase") == 1);
        CHECK(saw.find_param("trig") == 2);
        CHECK(saw.find_param("nonexistent") == -1);
    }

    SECTION("find_param by name - lp") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK(lp.find_param("in") == 0);
        CHECK(lp.find_param("cut") == 1);
        CHECK(lp.find_param("q") == 2);
        CHECK(lp.find_param("nonexistent") == -1);
    }

    SECTION("find_param returns -1 for empty param names") {
        const auto& clock = BUILTIN_FUNCTIONS.at("clock");
        CHECK(clock.find_param("") == -1);
        CHECK(clock.find_param("anything") == -1);
    }

    SECTION("has_default for required params") {
        const auto& saw = BUILTIN_FUNCTIONS.at("saw");
        CHECK_FALSE(saw.has_default(0));  // freq - required
    }

    SECTION("has_default for optional params") {
        const auto& sqr_pwm = BUILTIN_FUNCTIONS.at("sqr_pwm");
        CHECK_FALSE(sqr_pwm.has_default(0));  // freq - required
        CHECK_FALSE(sqr_pwm.has_default(1));  // pwm - required
        // phase and trig have NAN defaults
        CHECK_FALSE(sqr_pwm.has_default(2));  // phase - NAN means no default
        CHECK_FALSE(sqr_pwm.has_default(3));  // trig - NAN means no default
    }

    SECTION("has_default for lp filter") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK_FALSE(lp.has_default(0));  // in - required
        CHECK_FALSE(lp.has_default(1));  // cut - required
        CHECK(lp.has_default(2));        // q - optional with 0.707f default
    }

    SECTION("has_default index out of range") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        // Index >= MAX_BUILTIN_DEFAULTS + input_count
        CHECK_FALSE(lp.has_default(10));
    }

    SECTION("get_default returns correct values - moog") {
        const auto& moog = BUILTIN_FUNCTIONS.at("moog");
        CHECK(moog.get_default(2) == 1.0f);  // res default is 1.0
    }

    SECTION("get_default returns correct values - lp") {
        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK(lp.get_default(2) == 0.707f);  // q default is 0.707
    }

    SECTION("get_default returns correct values - adsr") {
        const auto& adsr = BUILTIN_FUNCTIONS.at("adsr");
        // adsr: gate required, attack=0.01, decay=0.1, sustain=0.5 (defaults in slots 0,1,2)
        CHECK(adsr.get_default(1) == 0.01f);  // attack
        CHECK(adsr.get_default(2) == 0.1f);   // decay
        CHECK(adsr.get_default(3) == 0.5f);   // sustain
    }

    SECTION("requires_state flag") {
        const auto& sine = BUILTIN_FUNCTIONS.at("sine");
        CHECK(sine.requires_state == true);

        const auto& lp = BUILTIN_FUNCTIONS.at("lp");
        CHECK(lp.requires_state == true);

        const auto& add = BUILTIN_FUNCTIONS.at("add");
        CHECK(add.requires_state == false);

        const auto& sin_fn = BUILTIN_FUNCTIONS.at("sin");
        CHECK(sin_fn.requires_state == false);
    }
}

TEST_CASE("lookup_builtin", "[builtins]") {
    SECTION("finds direct name - lp") {
        const auto* lp = lookup_builtin("lp");
        REQUIRE(lp != nullptr);
        CHECK(lp->opcode == cedar::Opcode::FILTER_SVF_LP);
    }

    SECTION("finds direct name - sine") {
        const auto* sine = lookup_builtin("sine");
        REQUIRE(sine != nullptr);
        CHECK(sine->opcode == cedar::Opcode::OSC_SIN);
    }

    SECTION("resolves alias - lowpass") {
        const auto* lowpass = lookup_builtin("lowpass");
        REQUIRE(lowpass != nullptr);
        CHECK(lowpass->opcode == cedar::Opcode::FILTER_SVF_LP);
    }

    SECTION("resolves alias - highpass") {
        const auto* highpass = lookup_builtin("highpass");
        REQUIRE(highpass != nullptr);
        CHECK(highpass->opcode == cedar::Opcode::FILTER_SVF_HP);
    }

    SECTION("resolves alias - reverb to freeverb") {
        const auto* reverb = lookup_builtin("reverb");
        REQUIRE(reverb != nullptr);
        CHECK(reverb->opcode == cedar::Opcode::REVERB_FREEVERB);
    }

    SECTION("resolves alias - tb303 to diode") {
        const auto* tb303 = lookup_builtin("tb303");
        REQUIRE(tb303 != nullptr);
        CHECK(tb303->opcode == cedar::Opcode::FILTER_DIODE);
    }

    SECTION("returns nullptr for unknown") {
        CHECK(lookup_builtin("not_a_builtin") == nullptr);
    }

    SECTION("returns nullptr for empty string") {
        CHECK(lookup_builtin("") == nullptr);
    }
}

TEST_CASE("canonical_name", "[builtins]") {
    SECTION("resolves alias to canonical - filters") {
        CHECK(canonical_name("lowpass") == "lp");
        CHECK(canonical_name("highpass") == "hp");
        CHECK(canonical_name("bandpass") == "bp");
    }

    SECTION("resolves alias to canonical - effects") {
        CHECK(canonical_name("reverb") == "freeverb");
        CHECK(canonical_name("plate") == "dattorro");
        CHECK(canonical_name("distort") == "saturate");
    }

    SECTION("resolves alias to canonical - dynamics") {
        // `compress` is now a pattern transform; only `compressor` aliases to `comp`.
        CHECK(canonical_name("compressor") == "comp");
        CHECK(canonical_name("limit") == "limiter");
    }

    SECTION("compress is no longer aliased to comp (Phase 2 PRD)") {
        // Pattern transform takes the name; audio users must use comp/compressor.
        CHECK(canonical_name("compress") == "compress");
    }

    SECTION("returns input for non-alias") {
        CHECK(canonical_name("lp") == "lp");
        CHECK(canonical_name("sine") == "sine");
        CHECK(canonical_name("unknown") == "unknown");
        CHECK(canonical_name("") == "");
    }
}
