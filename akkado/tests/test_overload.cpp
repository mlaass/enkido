// Overload-resolution model + resolver (PRD prd-builtin-overload-resolution).
//
// Phase 1: the pattern model + resolver. Codegen only drives the Type/Any
// matcher kinds via single-pattern builtins; the literal-value kinds
// (StringLiteral/NumberLiteral), multi-pattern resolve(), and closest_candidate()
// are implemented and unit-tested here ahead of their Phase 2/3 wiring.
//
// The codegen "no behavior change" guard lives mostly in
// test_param_type_annotations.cpp (Function→E160, sample/Record/Array passes).
// Here we add the StateCell case (otherwise untested) plus a smoke test that the
// resolver path leaves valid programs clean.

#include <catch2/catch_test_macros.hpp>
#include "akkado/overload.hpp"
#include "akkado/builtins.hpp"
#include "akkado/typed_value.hpp"
#include "akkado/akkado.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace akkado;

namespace {

ArgMatcher type_m(ParamValueType t) {
    ArgMatcher m;
    m.kind = ArgMatcher::Kind::Type;
    m.type = t;
    return m;
}
ArgMatcher any_m(bool reject_signal = false) {
    ArgMatcher m;
    m.kind = ArgMatcher::Kind::Any;
    m.reject_uncoercible_signal = reject_signal;
    return m;
}
ArgMatcher str_m(std::uint32_t id) {
    ArgMatcher m;
    m.kind = ArgMatcher::Kind::StringLiteral;
    m.string_id = id;
    return m;
}
ArgMatcher num_m(float v) {
    ArgMatcher m;
    m.kind = ArgMatcher::Kind::NumberLiteral;
    m.number = v;
    return m;
}

ArgDescriptor arg_t(ValueType t) {
    ArgDescriptor a;
    a.type = t;
    return a;
}
ArgDescriptor arg_str(std::uint32_t id) {
    ArgDescriptor a;
    a.type = ValueType::String;
    a.is_string_literal = true;
    a.string_id = id;
    return a;
}
ArgDescriptor arg_num(float v) {
    ArgDescriptor a;
    a.type = ValueType::Number;
    a.is_number_literal = true;
    a.number = v;
    return a;
}

DispatchPattern make_pattern(std::vector<ArgMatcher> ms, std::uint8_t required) {
    DispatchPattern p;
    p.params = std::move(ms);
    p.required_count = required;
    return p;
}

bool has_diagnostic(const akkado::CompileResult& r, std::string_view code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

}  // namespace

// -----------------------------------------------------------------------------
// matches_arg — the per-slot primitive.
// -----------------------------------------------------------------------------

TEST_CASE("matches_arg: Kind::Type mirrors the type_compatible matrix",
          "[overload]") {
    // Signal accepts Signal / Number / Pattern (coercion counts as a match).
    CHECK(matches_arg(type_m(ParamValueType::Signal), arg_t(ValueType::Signal)));
    CHECK(matches_arg(type_m(ParamValueType::Signal), arg_t(ValueType::Number)));
    CHECK(matches_arg(type_m(ParamValueType::Signal), arg_t(ValueType::Pattern)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::Signal), arg_t(ValueType::String)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::Signal), arg_t(ValueType::Array)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::Signal), arg_t(ValueType::Function)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::Signal), arg_t(ValueType::StateCell)));

    // Pattern only.
    CHECK(matches_arg(type_m(ParamValueType::Pattern), arg_t(ValueType::Pattern)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::Pattern), arg_t(ValueType::Signal)));

    // Record accepts Record or Pattern.
    CHECK(matches_arg(type_m(ParamValueType::Record), arg_t(ValueType::Record)));
    CHECK(matches_arg(type_m(ParamValueType::Record), arg_t(ValueType::Pattern)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::Record), arg_t(ValueType::Signal)));

    // String / Number / Function / Array are strict.
    CHECK(matches_arg(type_m(ParamValueType::String), arg_t(ValueType::String)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::String), arg_t(ValueType::Number)));
    CHECK(matches_arg(type_m(ParamValueType::Number), arg_t(ValueType::Number)));
    CHECK_FALSE(matches_arg(type_m(ParamValueType::Number), arg_t(ValueType::Signal)));
    CHECK(matches_arg(type_m(ParamValueType::Function), arg_t(ValueType::Function)));
    CHECK(matches_arg(type_m(ParamValueType::Array), arg_t(ValueType::Array)));
}

TEST_CASE("matches_arg: Kind::Any with no flag accepts everything", "[overload]") {
    const ValueType all[] = {
        ValueType::Signal,   ValueType::Number,  ValueType::Pattern,
        ValueType::Record,   ValueType::Array,   ValueType::String,
        ValueType::Function, ValueType::StateCell, ValueType::DynArray,
        ValueType::Stream,   ValueType::Void};
    for (ValueType t : all) {
        CHECK(matches_arg(any_m(/*reject*/ false), arg_t(t)));
    }
}

TEST_CASE("matches_arg: Kind::Any with reject rejects only Function/StateCell",
          "[overload]") {
    // The IMPLICIT signal-coerce slot: every coercible type passes...
    CHECK(matches_arg(any_m(/*reject*/ true), arg_t(ValueType::Signal)));
    CHECK(matches_arg(any_m(true), arg_t(ValueType::Number)));
    CHECK(matches_arg(any_m(true), arg_t(ValueType::Pattern)));
    CHECK(matches_arg(any_m(true), arg_t(ValueType::Array)));
    CHECK(matches_arg(any_m(true), arg_t(ValueType::Record)));
    CHECK(matches_arg(any_m(true), arg_t(ValueType::String)));
    // ...only the two with no audio meaning are rejected.
    CHECK_FALSE(matches_arg(any_m(true), arg_t(ValueType::Function)));
    CHECK_FALSE(matches_arg(any_m(true), arg_t(ValueType::StateCell)));
}

TEST_CASE("matches_arg: literal guards never match non-literals", "[overload]") {
    CHECK(matches_arg(str_m(42), arg_str(42)));
    CHECK_FALSE(matches_arg(str_m(42), arg_str(7)));
    CHECK_FALSE(matches_arg(str_m(42), arg_t(ValueType::String)));  // non-literal String
    CHECK_FALSE(matches_arg(str_m(42), arg_num(42)));

    CHECK(matches_arg(num_m(128.0f), arg_num(128.0f)));
    CHECK_FALSE(matches_arg(num_m(128.0f), arg_num(64.0f)));
    CHECK_FALSE(matches_arg(num_m(128.0f), arg_t(ValueType::Number)));  // non-literal Number
}

// -----------------------------------------------------------------------------
// matches — whole-pattern, arity-aware.
// -----------------------------------------------------------------------------

TEST_CASE("matches: arity gating against required_count and params size",
          "[overload]") {
    DispatchPattern p = make_pattern({any_m(), any_m(), any_m()}, /*required*/ 2);

    CHECK_FALSE(matches(p, {arg_t(ValueType::Signal)}));  // below required
    CHECK(matches(p, {arg_t(ValueType::Signal), arg_t(ValueType::Signal)}));
    CHECK(matches(p, {arg_t(ValueType::Signal), arg_t(ValueType::Signal),
                      arg_t(ValueType::Signal)}));
    CHECK_FALSE(matches(p, {arg_t(ValueType::Signal), arg_t(ValueType::Signal),
                            arg_t(ValueType::Signal),
                            arg_t(ValueType::Signal)}));  // above params size
}

TEST_CASE("matches: out_failures collects every failing slot", "[overload]") {
    DispatchPattern p = make_pattern(
        {type_m(ParamValueType::Signal), type_m(ParamValueType::Pattern)}, 2);

    std::vector<SlotFailure> failures;
    bool ok = matches(p, {arg_t(ValueType::String), arg_t(ValueType::String)},
                      &failures);
    CHECK_FALSE(ok);
    REQUIRE(failures.size() == 2);
    CHECK(failures[0].index == 0);
    CHECK(failures[0].matcher_kind == ArgMatcher::Kind::Type);
    CHECK(failures[0].expected == ParamValueType::Signal);
    CHECK(failures[1].index == 1);
    CHECK(failures[1].expected == ParamValueType::Pattern);
}

TEST_CASE("matches: skip slots always match", "[overload]") {
    DispatchPattern p = make_pattern({type_m(ParamValueType::Pattern)}, 1);
    ArgDescriptor skipped;
    skipped.type = ValueType::Function;  // would fail if checked
    skipped.skip = true;
    CHECK(matches(p, {skipped}));
}

// -----------------------------------------------------------------------------
// resolve — first match in declaration order; coercion counts.
// -----------------------------------------------------------------------------

TEST_CASE("resolve: first match wins; declaration order is the only knob",
          "[overload]") {
    DispatchPattern num = make_pattern({type_m(ParamValueType::Number)}, 1);
    DispatchPattern sig = make_pattern({type_m(ParamValueType::Signal)}, 1);
    std::vector<ArgDescriptor> args = {arg_t(ValueType::Number)};

    // (Number) before (Signal): a Number binds to the Number form.
    {
        std::vector<DispatchPattern> table = {num, sig};
        ResolveResult r = resolve(table, args);
        REQUIRE(r.matched);
        CHECK(r.closest_index == 0);
        CHECK(r.pattern->params[0].type == ParamValueType::Number);
    }
    // (Signal) before (Number): the same Number now coerces into Signal first.
    {
        std::vector<DispatchPattern> table = {sig, num};
        ResolveResult r = resolve(table, args);
        REQUIRE(r.matched);
        CHECK(r.closest_index == 0);
        CHECK(r.pattern->params[0].type == ParamValueType::Signal);
    }
}

TEST_CASE("resolve: coercion counts as a match for a Signal pattern",
          "[overload]") {
    std::vector<DispatchPattern> table = {
        make_pattern({type_m(ParamValueType::Signal)}, 1)};
    CHECK(resolve(table, {arg_t(ValueType::Number)}).matched);
    CHECK(resolve(table, {arg_t(ValueType::Pattern)}).matched);
    CHECK_FALSE(resolve(table, {arg_t(ValueType::String)}).matched);
}

// -----------------------------------------------------------------------------
// closest_candidate — fewest coercion-failing slots, then arity distance.
// -----------------------------------------------------------------------------

TEST_CASE("closest_candidate: prefers fewer failing slots then closer arity",
          "[overload]") {
    DispatchPattern two_sig = make_pattern(
        {type_m(ParamValueType::Signal), type_m(ParamValueType::Signal)}, 2);
    DispatchPattern one_sig =
        make_pattern({type_m(ParamValueType::Signal)}, 1);
    std::vector<DispatchPattern> table = {two_sig, one_sig};

    // arg String fails both signal slots equally (1 fail each over the overlap),
    // so arity distance decides: one_sig accepts arity 1 exactly (distance 0),
    // two_sig wants 2 (distance 1) → pick one_sig (index 1).
    std::size_t idx = closest_candidate(table, {arg_t(ValueType::String)});
    CHECK(idx == 1);

    ResolveResult r = resolve(table, {arg_t(ValueType::String)});
    CHECK_FALSE(r.matched);
    CHECK(r.closest_index == 1);
    REQUIRE_FALSE(r.failures.empty());
    CHECK(r.failures[0].expected == ParamValueType::Signal);
}

TEST_CASE("closest_candidate: ties resolve to the earliest declaration",
          "[overload]") {
    DispatchPattern a = make_pattern({type_m(ParamValueType::Signal)}, 1);
    DispatchPattern b = make_pattern({type_m(ParamValueType::Signal)}, 1);
    std::vector<DispatchPattern> table = {a, b};
    CHECK(closest_candidate(table, {arg_t(ValueType::String)}) == 0);
}

// -----------------------------------------------------------------------------
// make_builtin_pattern — synthesis from a real BuiltinInfo.
// -----------------------------------------------------------------------------

TEST_CASE("make_builtin_pattern: implicit signal slots for an oscillator",
          "[overload]") {
    const BuiltinInfo* sine = lookup_builtin("sine");
    REQUIRE(sine != nullptr);

    DispatchPattern p = make_builtin_pattern(*sine);
    CHECK(p.params.size() == MAX_BUILTIN_PARAMS);
    CHECK(p.required_count == sine->input_count);

    const std::size_t total = sine->total_params();
    REQUIRE(total > 0);
    REQUIRE(total < MAX_BUILTIN_PARAMS);
    for (std::size_t i = 0; i < MAX_BUILTIN_PARAMS; ++i) {
        CHECK(p.params[i].kind == ArgMatcher::Kind::Any);
        // Real parameter slots reject Function/StateCell; slots beyond the
        // builtin's arity are pure Any.
        CHECK(p.params[i].reject_uncoercible_signal == (i < total));
    }
}

TEST_CASE("make_builtin_pattern: explicit param_types become Type matchers",
          "[overload]") {
    // midi() carries an explicit param_types[0] = Record.
    const BuiltinInfo* midi = lookup_builtin("midi");
    REQUIRE(midi != nullptr);

    DispatchPattern p = make_builtin_pattern(*midi);
    CHECK(p.params[0].kind == ArgMatcher::Kind::Type);
    CHECK(p.params[0].type == ParamValueType::Record);
}

// -----------------------------------------------------------------------------
// Codegen integration — the resolver now drives the generic param_types check.
// -----------------------------------------------------------------------------

TEST_CASE("overload codegen: StateCell at an implicit signal slot is E160",
          "[overload]") {
    // A StateCell has no audio meaning at a coerced signal slot — no coercion
    // path, so the resolver rejects it (previously uncovered by tests).
    auto r = akkado::compile("s = state(0)\nsine(s) |> out(@)");
    CHECK(has_diagnostic(r, "E160"));
}

TEST_CASE("overload codegen: a clean program leaves no spurious E160",
          "[overload]") {
    auto r = akkado::compile("sine(440) |> out(@)");
    CHECK(r.success);
    CHECK_FALSE(has_diagnostic(r, "E160"));
}
