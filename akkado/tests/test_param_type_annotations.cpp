// Parameter type annotations for user-defined `fn` (PRD prd-parameter-type-annotations).
//
// Type names are uppercase PascalCase identifiers mirroring the C++ ValueType
// enum: `Signal`, `Number`, `Pattern`, `Record`, `Array`, `String`,
// `Function`, `Stream`. They are not keywords — they lex as identifiers and are
// resolved contextually only in annotation position (after a `:`).
//   - `: Stream`  -- abstract supertype: Pattern (incl. runtime event streams)
//   - `: Pattern` -- strict Pattern
//   - `: Signal`  -- explicit form of the implicit voice-0 coerce
//
// This file holds:
//   [type-annotation] -- unit tests for the type_compatible() matrix and the
//                        codegen-side E184 boundary checks (§4.2 of the PRD).

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
    // §4.2 row: `: Stream` accepts Pattern (mono + poly, MIDI-pattern, and
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
// `: Stream` / `: Signal` branches in handle_user_function_call. Verifies that
// the boundary check fires E184 / preserves E160 / bypasses E160 as designed
// in §4.2.
// =============================================================================

TEST_CASE(": Stream accepts Pattern arguments (mono and polyphonic)",
          "[type-annotation][codegen]") {
    SECTION("mono Pattern passes through without E160") {
        auto r = akkado::compile(R"(
            fn id(e: Stream) -> e.freq
            n"c4 e4 g4" |> id(@) |> sine(@) |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }

    SECTION("polyphonic Pattern (chord) passes through — E160 bypass") {
        // Headline behavior: without the `: Stream` annotation, the same call
        // would fire E160 (and indeed does in the un-annotated test below).
        // `c"Am"` is the canonical chord-pattern source — max_voices=3.
        auto r = akkado::compile(R"(
            fn id(e: Stream) -> e
            id(c"Am")
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }
}

TEST_CASE(": Stream rejects non-Pattern / non-EventSource args with E184",
          "[type-annotation][codegen]") {
    SECTION("Number → E184") {
        auto r = akkado::compile(R"(
            fn id(e: Stream) -> e
            id(440)
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'e'"));
        CHECK_FALSE(has_diagnostic(r, "E160"));
    }

    SECTION("String → E184") {
        auto r = akkado::compile(R"(
            fn id(e: Stream) -> e
            id("text")
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'e'"));
    }

    SECTION("Signal → E184") {
        auto r = akkado::compile(R"(
            fn id(e: Stream) -> e
            id(sine(440))
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

TEST_CASE(": Signal preserves E160 for poly Pattern, allows mono coerce",
          "[type-annotation][codegen]") {
    SECTION("Number arg accepted (today's behavior, made explicit)") {
        auto r = akkado::compile(R"(
            fn w(rate: Signal) -> sine(rate)
            w(220) |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }

    SECTION("mono Pattern arg silently voice-0 coerces") {
        auto r = akkado::compile(R"(
            fn w(rate: Signal) -> sine(rate)
            w(n"c4 e4") |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
        CHECK_FALSE(has_diagnostic(r, "E184"));
    }

    SECTION("polyphonic Pattern fires E160 (preserved reject from PRD §4.2)") {
        auto r = akkado::compile(R"(
            fn w(rate: Signal) -> sine(rate)
            w(c"Am")
        )");
        CHECK(has_diagnostic_for_param(r, "E160", "'rate'"));
    }

    SECTION("String arg fires E184 (no coercion path)") {
        // String → Signal has no defensible coerce path per PRD §4.2.
        auto r = akkado::compile(R"(
            fn w(rate: Signal) -> sine(rate)
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

TEST_CASE(": Stream-annotated param exposes Pattern field access in the body",
          "[type-annotation][codegen]") {
    // PRD §4.3: the Stream-annotated param preserves the caller's TypedValue
    // across the boundary so `events.freq` works inside the body — this is
    // the DynArray-template binding path (codegen_functions.cpp Loop 2). The
    // pre-PRD behavior collapsed Pattern → voice-0 scalar and lost the
    // field-accessor map.
    auto r = akkado::compile(R"(
        fn carrier(events: Stream) -> sine(events.freq)
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
// half is run out of band against ./build/bin/nkido render
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

TEST_CASE("e2e §10.3: xp(events: Stream, n) compiles for mono Pattern",
          "[type-annotation][e2e]") {
    auto r = akkado::compile(R"(
        fn xp(events: Stream, n) ->
            event_map(events, (e) -> {note: e.note + n})

        n"c4 e4 g4".xp(7) |> sine(@.freq) |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

TEST_CASE("e2e §10.3: xp(events: Stream, n) compiles for chord-stack pattern",
          "[type-annotation][e2e]") {
    // PRD §10.3 second program (adapted). The point is that the user fn's
    // `events: Stream` param accepts a chord-stack pattern without an
    // E160 / E184 boundary diagnostic.
    auto r = akkado::compile(R"(
        fn xp(events: Stream, n) ->
            event_map(events, (e) -> {note: e.note + n})

        n"[c4,e4,g4]".xp(7)
          |> poly(@, (f, g, v) -> sine(f) * adsr(g, 0.01, 0.1, 0.5, 0.2) * v, 3)
          |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

TEST_CASE("e2e §10.3: xp(events: Stream, n) accepts true polyphonic chord pattern",
          "[type-annotation][e2e]") {
    // Sharper version of the chord-stack test: c"Am" is a true polyphonic
    // chord (max_voices=3), so this is the exact scenario the un-annotated
    // handle_user_function_call rejects with E160. With the `: Stream`
    // annotation, the bypass is the headline behavior — this test ships as a
    // regression guard.
    auto r = akkado::compile(R"(
        fn xp(events: Stream, n) ->
            event_map(events, (e) -> {note: e.note + n})

        c"Am".xp(7)
          |> poly(@, (f, g, v) -> sine(f) * v, 3)
          |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
}

// =============================================================================
// PRD prd-parameter-type-annotations §10.2: codegen-side tests for the
// `: Number`, `: Record`, `: Array`, `: String`, and `: Function` annotations.
// =============================================================================

TEST_CASE("type_compatible: ParamValueType::Number accepts only Number",
          "[type-annotation][type-compat]") {
    CHECK(type_compatible(ValueType::Number, ParamValueType::Number));
    CHECK_FALSE(type_compatible(ValueType::Signal,   ParamValueType::Number));
    CHECK_FALSE(type_compatible(ValueType::Pattern,  ParamValueType::Number));
    CHECK_FALSE(type_compatible(ValueType::Record,   ParamValueType::Number));
    CHECK_FALSE(type_compatible(ValueType::Array,    ParamValueType::Number));
    CHECK_FALSE(type_compatible(ValueType::DynArray, ParamValueType::Number));
    CHECK_FALSE(type_compatible(ValueType::String,   ParamValueType::Number));
    CHECK_FALSE(type_compatible(ValueType::Function, ParamValueType::Number));
}

TEST_CASE("param_value_type_name includes Number",
          "[type-annotation][type-compat]") {
    CHECK(std::string(param_value_type_name(ParamValueType::Number)) == "Number");
}

// ----- : Number -----

TEST_CASE(": Number accepts Number argument",
          "[type-annotation][codegen]") {
    auto r = akkado::compile(R"(
        fn pickn(x: Number) -> x
        pickn(5)
    )");
    CHECK_FALSE(has_diagnostic(r, "E184"));
}

TEST_CASE(": Number rejects non-Number args with E184",
          "[type-annotation][codegen]") {
    SECTION("Signal → E184") {
        auto r = akkado::compile(R"(
            fn pickn(x: Number) -> x
            pickn(sine(1))
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'x'"));
    }
    SECTION("Pattern → E184") {
        auto r = akkado::compile(R"(
            fn pickn(x: Number) -> x
            pickn(n"c4 e4")
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'x'"));
    }
    SECTION("String → E184") {
        auto r = akkado::compile(R"(
            fn pickn(x: Number) -> x
            pickn("text")
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'x'"));
    }
}

// ----- : Record -----

TEST_CASE(": Record accepts Record argument",
          "[type-annotation][codegen]") {
    auto r = akkado::compile(R"(
        fn freqof(r: Record) -> r.freq
        freqof({freq: 440, vel: 0.8})
    )");
    CHECK_FALSE(has_diagnostic(r, "E184"));
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

TEST_CASE(": Record accepts Pattern argument and exposes field access",
          "[type-annotation][codegen]") {
    // PRD §8.6: Pattern is structurally a record. The binding-selection
    // branch (codegen_functions.cpp ~842) preserves the Pattern TypedValue
    // so `r.freq` resolves via the Pattern's per-field buffers.
    auto r = akkado::compile(R"(
        fn freqof(r: Record) -> r.freq
        n"c4 e4" |> freqof(@) |> sine(@) |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E160"));
    CHECK_FALSE(has_diagnostic(r, "E184"));
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

TEST_CASE(": Record rejects non-Record/non-Pattern args with E184",
          "[type-annotation][codegen]") {
    SECTION("Number → E184") {
        auto r = akkado::compile(R"(
            fn freqof(r: Record) -> r.freq
            freqof(220)
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'r'"));
    }
    SECTION("Array → E184") {
        auto r = akkado::compile(R"(
            fn freqof(r: Record) -> r.freq
            freqof([1, 2, 3])
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'r'"));
    }
}

// ----- : Array -----

TEST_CASE(": Array accepts Array literal",
          "[type-annotation][codegen]") {
    auto r = akkado::compile(R"(
        fn pick0(a: Array) -> a[0]
        pick0([220, 440, 880]) |> sine(@) |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E184"));
}

TEST_CASE(": Array rejects DynArray with E184 and DynArray-specific hint",
          "[type-annotation][codegen]") {
    auto r = akkado::compile(R"(
        fn pick0(a: Array) -> a[0]
        n"c4 e4 g4" as ev |> pick0(notes(ev))
    )");
    CHECK(has_diagnostic_for_param(r, "E184", "'a'"));
    // The hint substring distinguishes the DynArray rejection from the
    // generic "expects Array" case.
    bool has_dynarray_hint = false;
    for (const auto& d : r.diagnostics) {
        if (d.code == "E184" && d.message.find("DynArray") != std::string::npos) {
            has_dynarray_hint = true;
        }
    }
    CHECK(has_dynarray_hint);
}

TEST_CASE(": Array rejects Record with E184",
          "[type-annotation][codegen]") {
    auto r = akkado::compile(R"(
        fn pick0(a: Array) -> a[0]
        pick0({x: 1, y: 2})
    )");
    CHECK(has_diagnostic_for_param(r, "E184", "'a'"));
}

// ----- : String -----

TEST_CASE(": String accepts String literal",
          "[type-annotation][codegen]") {
    auto r = akkado::compile(R"(
        fn osctype(kind: String, freq) -> osc(kind, freq)
        osctype("saw", 440) |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E184"));
}

TEST_CASE(": String rejects non-String args with E184",
          "[type-annotation][codegen]") {
    SECTION("Number → E184") {
        auto r = akkado::compile(R"(
            fn osctype(kind: String, freq) -> osc(kind, freq)
            osctype(220, 440)
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'kind'"));
    }
    SECTION("Pattern → E184") {
        auto r = akkado::compile(R"(
            fn osctype(kind: String, freq) -> osc(kind, freq)
            osctype(n"c4", 440)
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'kind'"));
    }
}

// ----- : Function -----

TEST_CASE(": Function accepts a closure literal",
          "[type-annotation][codegen]") {
    auto r = akkado::compile(R"(
        fn apply1(cb: Function) -> cb(1)
        apply1((x) -> x * 2)
    )");
    CHECK_FALSE(has_diagnostic(r, "E184"));
}

TEST_CASE(": Function rejects non-Function args with E184",
          "[type-annotation][codegen]") {
    SECTION("Number → E184") {
        auto r = akkado::compile(R"(
            fn apply1(cb: Function) -> cb(1)
            apply1(220)
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'cb'"));
    }
    SECTION("String → E184") {
        auto r = akkado::compile(R"(
            fn apply1(cb: Function) -> cb(1)
            apply1("saw")
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'cb'"));
    }
}

// ----- : Pattern (strict Pattern, distinct from Stream) -----

TEST_CASE(": Pattern accepts a Pattern argument",
          "[type-annotation][codegen]") {
    // ParamValueType::Pattern is the strict form: accepts Pattern only (unlike
    // Stream, which is the abstract supertype, or Signal, which voice-0
    // coerces). A mono note pattern is the canonical accepted case.
    auto r = akkado::compile(R"(
        fn play(p: Pattern) -> p.freq
        n"c4 e4 g4" |> play(@) |> sine(@) |> out(@)
    )");
    CHECK_FALSE(has_diagnostic(r, "E184"));
    CHECK_FALSE(has_diagnostic(r, "E136"));
}

TEST_CASE(": Pattern rejects non-Pattern args with E184",
          "[type-annotation][codegen]") {
    SECTION("Number → E184") {
        auto r = akkado::compile(R"(
            fn play(p: Pattern) -> p
            play(440)
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'p'"));
    }
    SECTION("Signal → E184") {
        auto r = akkado::compile(R"(
            fn play(p: Pattern) -> p
            play(sine(440))
        )");
        CHECK(has_diagnostic_for_param(r, "E184", "'p'"));
    }
}

// =============================================================================
// PRD prd-compiler-type-system Phase 4: builtin-argument type checking.
//
// Two modes:
//   - Implicit (args_are_signal default): coerce-friendly. A signal slot
//     accepts Signal/Number/Pattern/Array/Record/String — anything with a
//     defensible signal interpretation ([[feedback_livecoding_coerce_dont_
//     fail]]). Only Function / StateCell (no audio meaning) are rejected.
//   - Explicit annotation (out, visualizers): strict — type_compatible decides.
// =============================================================================

TEST_CASE("builtin arg check: Function at an implicit signal slot is E160",
          "[type-annotation][builtin-args]") {
    // A bound function passed where a signal is expected has no coercion path.
    auto r = akkado::compile(R"(
        f = (x) -> x * 2
        sine(f) |> out(@)
    )");
    CHECK(has_diagnostic(r, "E160"));
}

TEST_CASE("builtin arg check: coerce-friendly types pass at implicit signal slots",
          "[type-annotation][builtin-args]") {
    SECTION("String sample id passes (no false E160)") {
        // sample()'s id slot legitimately accepts a String name.
        auto r = akkado::compile(R"(sample(1.0, 1.0, "bd") |> out(@))");
        CHECK_FALSE(has_diagnostic(r, "E160"));
    }
    SECTION("event Record used as a scalar coerces (no false E160)") {
        // each_voice binds the lambda param to the event Record; using it
        // directly as a frequency coerces to its .freq buffer.
        auto r = akkado::compile(
            R"(n"c4 e4 g4" |> each_voice(@, (v) -> osc("sin", v) * 0.3) |> out(@))");
        CHECK_FALSE(has_diagnostic(r, "E160"));
    }
    SECTION("map-over-array expansion passes (no false E160)") {
        // triad is a static Array; saw() clones per element (multi-buffer
        // expansion), then sum() collapses. The Array is legitimate here.
        auto r = akkado::compile(R"(
            n"c4 e4 g4" as e
            triad = map([0, 4, 7], (i) -> e.note + i)
            triad.mtof().saw().sum() |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
    }
}

TEST_CASE("builtin arg check: explicit Signal annotation stays strict (Array → E160)",
          "[type-annotation][builtin-args]") {
    // out() has an explicit {Signal, Signal} annotation, so an Array (which
    // does not expand into out, a sink) is a hard type error — distinct from
    // the coerce-friendly implicit path above.
    auto r = akkado::compile("out([1, 2, 3])");
    CHECK_FALSE(r.success);
    CHECK(has_diagnostic(r, "E160"));
}

TEST_CASE("transport(): first argument must be a Pattern",
          "[type-annotation][builtin-args]") {
    // transport carries an explicit param_types[0] = Pattern. Its dedicated
    // handler (handle_transport_call) rejects a non-pattern first arg with E133
    // before the generic path; either way a Signal in slot 0 is a hard error.
    SECTION("Signal first arg is rejected") {
        auto r = akkado::compile("transport(sine(440), trigger(2)) |> out(@)");
        CHECK_FALSE(r.success);
        CHECK(has_diagnostic(r, "E133"));
    }
    SECTION("a real Pattern first arg compiles") {
        auto r = akkado::compile(
            R"(n"c4 e4 g4" |> transport(@, trigger(2)) |> sine(@) |> out(@))");
        CHECK(r.success);
    }
}

// =============================================================================
// PRD prd-compiler-type-system Phase 4: runtime match-arm type agreement.
// The SELECT chain merges every arm into one signal buffer, so arms must yield
// the same kind of value. Signal / Number / Pattern are mutually compatible
// (all reduce to a signal buffer); other types must match exactly.
// =============================================================================

TEST_CASE("match arms: mixing a Signal arm with a String arm is E160",
          "[type-annotation][match-arms]") {
    // Runtime match (non-const signal scrutinee `x`). One arm is a Signal, the
    // other a String — incompatible kinds the SELECT chain cannot merge.
    auto r = akkado::compile(R"(
        x = saw(1)
        match(x) {
            0: sine(440),
            _: "saw"
        } |> out(@)
    )");
    CHECK(has_diagnostic(r, "E160"));
}

TEST_CASE("match arms: uniform signal-like arms compile clean",
          "[type-annotation][match-arms]") {
    SECTION("all Signal arms") {
        auto r = akkado::compile(R"(
            x = saw(1)
            match(x) {
                0: sine(440),
                _: saw(220)
            } |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
    }
    SECTION("Signal and Number arms mix freely (both signal-like)") {
        auto r = akkado::compile(R"(
            x = saw(1)
            match(x) {
                0: sine(440),
                _: 0
            } |> out(@)
        )");
        CHECK_FALSE(has_diagnostic(r, "E160"));
    }
}
