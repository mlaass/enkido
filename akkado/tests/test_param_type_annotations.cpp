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
#include "akkado/builtins.hpp"
#include "akkado/typed_value.hpp"

using namespace akkado;

TEST_CASE("type_compatible: ParamValueType::Stream accepts only Pattern and EventSource",
          "[type-annotation][type-compat]") {
    // §4.2 row: `: stream` accepts Pattern (mono + poly, MIDI-pattern) and EventSource.
    CHECK(type_compatible(ValueType::Pattern,     ParamValueType::Stream));
    CHECK(type_compatible(ValueType::EventSource, ParamValueType::Stream));

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

    CHECK_FALSE(type_compatible(ValueType::EventSource, ParamValueType::Signal));
    CHECK_FALSE(type_compatible(ValueType::Pattern,     ParamValueType::Function));
    CHECK_FALSE(type_compatible(ValueType::Record,      ParamValueType::Array));
}

TEST_CASE("value_type_name and param_value_type_name include Stream",
          "[type-annotation][type-compat]") {
    // Diagnostic-formatting helpers must round-trip the new variant so error
    // messages can say "expects Stream" / "got Stream".
    CHECK(std::string(value_type_name(ValueType::Stream))     == "Stream");
    CHECK(std::string(param_value_type_name(ParamValueType::Stream)) == "Stream");
}
