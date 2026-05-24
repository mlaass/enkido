// Parameter type annotations for user-defined `fn` (PRD prd-parameter-type-annotations).
//
// Phase 1 ships two annotation keywords on fn parameter lists:
//   - `: stream`  -- abstract supertype: Pattern OR EventSource
//   - `: signal`  -- explicit form of today's implicit voice-0 coerce
//
// This file holds:
//   [type-annotation] -- Commit A unit tests for the ParamValueType::Stream
//                        compatibility matrix (§4.2 of the PRD).
//
// Later commits append parser-grammar and codegen-binding cases here under
// the same tag.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include "akkado/builtins.hpp"
#include "akkado/typed_value.hpp"
#include <string>
#include <string_view>

using namespace akkado;

namespace {

bool has_diagnostic(const akkado::CompileResult& r, std::string_view code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

bool has_diagnostic_for_param(const akkado::CompileResult& r,
                               std::string_view code,
                               std::string_view param_name) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code && d.message.find(param_name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("type_compatible: ParamValueType::Stream accepts only Pattern",
          "[type-annotation][type-compat]") {
    // §4.2 row: `: stream` accepts Pattern (mono + poly, MIDI-pattern, and
    // runtime event streams via `is_runtime_event_source`). Phase 5 Commit I
    // collapsed the standalone EventSource discriminator into PatternPayload.
    CHECK(type_compatible(ValueType::Pattern, ParamValueType::Stream));

    // Every other ValueType is rejected. The codegen branch turns these
    // rejections into E184 diagnostics; the lookup itself is pure.
    CHECK_FALSE(type_compatible(ValueType::Signal,    ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::Number,    ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::Record,    ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::Array,     ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::String,    ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::Function,  ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::StateCell, ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::DynArray,  ParamValueType::Stream));
    CHECK_FALSE(type_compatible(ValueType::Void,      ParamValueType::Stream));

    // The Stream variant on the value side is the abstract supertype —
    // no codegen path ever produces TypedValue{Stream}. This row guards
    // against accidental "Stream is its own subtype" logic creeping in.
    CHECK_FALSE(type_compatible(ValueType::Stream, ParamValueType::Stream));
}

TEST_CASE("type_compatible: existing ParamValueType rows unchanged after Stream addition",
          "[type-annotation][type-compat]") {
    // Regression guard for the type_compatible() switch — Commit A added the
    // Stream case but must not have changed any of the existing rows.
    CHECK(type_compatible(ValueType::Signal,  ParamValueType::Any));
    CHECK(type_compatible(ValueType::Signal,  ParamValueType::Signal));
    CHECK(type_compatible(ValueType::Number,  ParamValueType::Signal));
    CHECK(type_compatible(ValueType::Pattern, ParamValueType::Signal));
    CHECK(type_compatible(ValueType::Pattern, ParamValueType::Pattern));
    CHECK(type_compatible(ValueType::Pattern, ParamValueType::Record));
    CHECK(type_compatible(ValueType::Record,  ParamValueType::Record));
    CHECK(type_compatible(ValueType::Array,   ParamValueType::Array));
    CHECK(type_compatible(ValueType::String,  ParamValueType::String));
    CHECK(type_compatible(ValueType::Function, ParamValueType::Function));

    CHECK_FALSE(type_compatible(ValueType::DynArray, ParamValueType::Signal));
    CHECK_FALSE(type_compatible(ValueType::Pattern,  ParamValueType::Function));
    CHECK_FALSE(type_compatible(ValueType::Record,   ParamValueType::Array));
}

TEST_CASE("value_type_name and param_value_type_name include Stream",
          "[type-annotation][type-compat]") {
    // Diagnostic-formatting helpers must round-trip the new variant so error
    // messages can say "expects Stream" / "got Stream".
    CHECK(std::string(value_type_name(ValueType::Stream))     == "Stream");
    CHECK(std::string(param_value_type_name(ParamValueType::Stream)) == "Stream");
}

// =============================================================================
// PRD prd-parameter-type-annotations §10.2: codegen-side tests for the new
// `: stream` / `: signal` branches in handle_user_function_call. Verifies that
// the boundary check fires E184 / preserves E160 / bypasses E160 as designed
// in §4.2.
// =============================================================================

TEST_CASE(": stream accepts Pattern arguments (mono and polyphonic)",
          "[type-annotation][codegen]") {
    SECTION("mono Pattern passes through without E160") {
        auto r = akkado::compile(R"(
            fn id(e: stream) -> e.freq
            n"c4 e4 g4" |> id(@) |> osc("sin", @) |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }

    SECTION("polyphonic Pattern (chord) passes through — E160 bypass") {
        // Headline behavior: without the `: stream` annotation, the same call
        // would fire E160 (and indeed does in the un-annotated test below).
        // `c"Am"` is the canonical chord-pattern source — max_voices=3.
        auto r = akkado::compile(R"(
            fn id(e: stream) -> e
            id(c"Am")
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }
}

TEST_CASE(": stream rejects non-Pattern / non-EventSource args with E184",
          "[type-annotation][codegen]") {
    SECTION("Number → E184") {
        auto r = akkado::compile(R"(
            fn id(e: stream) -> e
            id(440)
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'e'"));
        CHECK_FALSE(has_diagnostic(r, "E160"));
    }

    SECTION("String → E184") {
        auto r = akkado::compile(R"(
            fn id(e: stream) -> e
            id("text")
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'e'"));
    }

    SECTION("Signal → E184") {
        auto r = akkado::compile(R"(
            fn id(e: stream) -> e
            id(osc("sin", 440))
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'e'"));
    }
}

TEST_CASE("un-annotated user-fn params keep existing E160 / coerce behavior",
          "[type-annotation][codegen]") {
    SECTION("polyphonic Pattern still rejected with E160 (no annotation)") {
        // Regression guard: the PRD §11 R2-Q4 contract — un-annotated params
        // keep bit-for-bit behavior. c"Am" is a polyphonic chord pattern.
        auto r = akkado::compile(R"(
            fn id(p) -> p
            id(c"Am")
        )");
        CHECK(has_diagnostic_for_param(r, "E160", "'p'"));
    }

    SECTION("mono Pattern silently voice-0 coerces (no annotation)") {
        auto r = akkado::compile(R"(
            fn id(p) -> p
            id(n"c4 e4")
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }
}

TEST_CASE(": signal preserves E160 for poly Pattern, allows mono coerce",
          "[type-annotation][codegen]") {
    SECTION("Number arg accepted (today's behavior, made explicit)") {
        auto r = akkado::compile(R"(
            fn w(rate: signal) -> osc("sin", rate)
            w(220) |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }

    SECTION("mono Pattern arg silently voice-0 coerces") {
        auto r = akkado::compile(R"(
            fn w(rate: signal) -> osc("sin", rate)
            w(n"c4 e4") |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }

    SECTION("polyphonic Pattern fires E160 (preserved reject from PRD §4.2)") {
        auto r = akkado::compile(R"(
            fn w(rate: signal) -> osc("sin", rate)
            w(c"Am")
        )");
        CHECK(has_diagnostic_for_param(r, "E160", "'rate'"));
    }

    SECTION("String arg fires E184 (no coercion path)") {
        // String → Signal has no defensible coerce path per PRD §4.2.
        auto r = akkado::compile(R"(
            fn w(rate: signal) -> osc("sin", rate)
            w("text")
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'rate'"));
    }

    // NOTE: `midi(...)` returns a Pattern with is_runtime_event_source=true
    // (MIDI-as-Pattern parity, PRD §4.1). Phase 5 Commit I removed the
    // standalone TypedValue{EventSource} discriminator — all event streams
    // now ride on Pattern with the flag set, so there is no longer a
    // separate codegen path to exercise here.
}

TEST_CASE(": stream-annotated param exposes Pattern field access in the body",
          "[type-annotation][codegen]") {
    // PRD §4.3: the Stream-annotated param preserves the caller's TypedValue
    // across the boundary so `events.freq` works inside the body — this is
    // the DynArray-template binding path (codegen_functions.cpp Loop 2). The
    // pre-PRD behavior collapsed Pattern → voice-0 scalar and lost the
    // field-accessor map.
    auto r = akkado::compile(R"(
        fn carrier(events: stream) -> osc("sin", events.freq)
        n"c4 e4 g4" |> carrier(@) |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
    // Body-side field lookup ("freq") must resolve — if it doesn't, an E13x
    // diagnostic would surface from the field-access machinery.
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

// =============================================================================
// PRD prd-parameter-type-annotations §10.3: end-to-end verification examples.
// The acceptance check is "compiles clean and the resulting WAV has the right
// peaks." This file ships the compile-only half (Catch2); the render-and-peak
// half is run out of band against ./build/tools/nkido-cli/nkido-cli render
// (see the commit message for the render result).
//
// These programs unblock prd-runtime-event-transforms.md Phase 2b — once they
// compile, the stdlib `event_transforms.ak` migration (transpose / velocity /
// etc. as one-liners over event_map) becomes syntactically expressible.
// =============================================================================

// Note: the PRD §10.3 examples use `fn name(...) = body` syntax, but akkado
// uses `fn name(...) -> body`. The tests below adapt the §10.3 programs to
// the actual grammar while preserving their semantics (the user-defined
// `xp` fn delegates straight to event_map, exactly as the PRD's `transpose`
// stdlib migration target does).
//
// We pick `xp` rather than `transpose` because `transpose` is already a
// builtin in this build (prd-runtime-event-transforms Phase 1) — shadowing
// it from userspace conflicts with the existing definition.

TEST_CASE("e2e §10.3: xp(events: stream, n) compiles for mono Pattern",
          "[type-annotation][e2e]") {
    auto r = akkado::compile(R"(
        fn xp(events: stream, n) ->
            event_map(events, (e) -> {note: e.note + n})

        n"c4 e4 g4".xp(7) |> osc("sin", @.freq) |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

TEST_CASE("e2e §10.3: xp(events: stream, n) compiles for chord-stack pattern",
          "[type-annotation][e2e]") {
    // PRD §10.3 second program (adapted). The point is that the user fn's
    // `events: stream` param accepts a chord-stack pattern without an
    // E160 / E184 boundary diagnostic.
    auto r = akkado::compile(R"(
        fn xp(events: stream, n) ->
            event_map(events, (e) -> {note: e.note + n})

        n"[c4,e4,g4]".xp(7)
          |> poly(@, (f, g, v) -> osc("sin", f) * adsr(g, 0.01, 0.1, 0.5, 0.2) * v, 3)
          |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

TEST_CASE("e2e §10.3: xp(events: stream, n) accepts true polyphonic chord pattern",
          "[type-annotation][e2e]") {
    // Sharper version of the chord-stack test: c"Am" is a true polyphonic
    // chord (max_voices=3), so this is the exact scenario the un-annotated
    // handle_user_function_call rejects with E160. With the `: stream`
    // annotation, the bypass is the headline behavior — this test ships as a
    // regression guard.
    auto r = akkado::compile(R"(
        fn xp(events: stream, n) ->
            event_map(events, (e) -> {note: e.note + n})

        c"Am".xp(7)
          |> poly(@, (f, g, v) -> osc("sin", f) * v, 3)
          |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
}
