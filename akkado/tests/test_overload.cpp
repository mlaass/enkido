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
#include <cedar/vm/instruction.hpp>
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

// Scan compiled bytecode for an opcode (Phase 4 selection tests use distinct
// builtins per overload, so the inlined body is observable in the opcode stream).
std::size_t count_opcode(const akkado::CompileResult& r, cedar::Opcode target) {
    const auto* inst = reinterpret_cast<const cedar::Instruction*>(r.bytecode.data());
    std::size_t n = r.bytecode.size() / sizeof(cedar::Instruction);
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (inst[i].opcode == target) ++count;
    }
    return count;
}
bool has_opcode(const akkado::CompileResult& r, cedar::Opcode target) {
    return count_opcode(r, target) > 0;
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

// -----------------------------------------------------------------------------
// Phase 2 — operators as named builtins (lookup_operator_overloads).
// -----------------------------------------------------------------------------

TEST_CASE("operator overloads: every operator name resolves to one pattern",
          "[overload]") {
    const std::string_view ops[] = {
        "add", "sub", "mul", "div", "pow",      // arithmetic  + - * / ^
        "eq", "neq", "lt", "gt", "lte", "gte",  // comparison  == != < > <= >=
        "band", "bor", "bnot",                  // logical     && || !
    };
    for (std::string_view nm : ops) {
        const auto* patterns = lookup_operator_overloads(nm);
        REQUIRE(patterns != nullptr);
        CHECK(patterns->size() == 1);  // single pattern per operator in Phase 2
    }
}

TEST_CASE("operator overloads: non-operators return nullptr", "[overload]") {
    // Plain builtins keep the generic path — they are not operators.
    CHECK(lookup_operator_overloads("sine") == nullptr);
    CHECK(lookup_operator_overloads("min") == nullptr);
    // neg (unary minus) and fmod (%) are functions only — no such operators
    // exist in Akkado, so they are deliberately out of the operator table.
    CHECK(lookup_operator_overloads("neg") == nullptr);
    CHECK(lookup_operator_overloads("fmod") == nullptr);
    CHECK(lookup_operator_overloads("nonexistent_fn") == nullptr);
}

TEST_CASE("operator overloads: arithmetic ops carry a LegacyHandler target",
          "[overload]") {
    for (std::string_view nm : {"add", "sub", "mul", "div", "pow"}) {
        const auto* patterns = lookup_operator_overloads(nm);
        REQUIRE(patterns != nullptr);
        REQUIRE(patterns->size() == 1);
        const DispatchTarget& t = (*patterns)[0].target;
        CHECK(t.kind == DispatchTarget::Kind::LegacyHandler);
        CHECK(t.legacy_handler == LegacyHandlerId::BinaryOpBroadcast);
    }
}

TEST_CASE("operator overloads: comparison/logical ops carry a Builtin target",
          "[overload]") {
    for (std::string_view nm : {"eq", "neq", "lt", "gt", "lte", "gte",
                                "band", "bor", "bnot"}) {
        const auto* patterns = lookup_operator_overloads(nm);
        REQUIRE(patterns != nullptr);
        REQUIRE(patterns->size() == 1);
        const DispatchTarget& t = (*patterns)[0].target;
        CHECK(t.kind == DispatchTarget::Kind::Builtin);
        CHECK(t.builtin == lookup_builtin(nm));
    }
}

TEST_CASE("operator overloads: patterns mirror make_builtin_pattern matchers",
          "[overload]") {
    // The matchers/required_count must equal the synthesized builtin pattern so
    // the generic per-arg E160 check stays byte-identical for the operators that
    // still flow through it (comparison/logical). Check a binary op and a unary.
    for (std::string_view nm : {"add", "eq", "bnot"}) {
        const auto* patterns = lookup_operator_overloads(nm);
        REQUIRE(patterns != nullptr);
        const DispatchPattern& op = (*patterns)[0];
        const BuiltinInfo* info = lookup_builtin(nm);
        REQUIRE(info != nullptr);
        DispatchPattern ref = make_builtin_pattern(*info);

        CHECK(op.required_count == ref.required_count);
        REQUIRE(op.params.size() == ref.params.size());
        for (std::size_t i = 0; i < op.params.size(); ++i) {
            CHECK(op.params[i].kind == ref.params[i].kind);
            CHECK(op.params[i].type == ref.params[i].type);
            CHECK(op.params[i].reject_uncoercible_signal ==
                  ref.params[i].reject_uncoercible_signal);
        }
    }
}

TEST_CASE("operator overloads: resolve() first-match extension point (Phase 3)",
          "[overload]") {
    // Forward-looking guard: once an operator gains a second pattern, resolve()
    // selects by declaration order with coercion counting as a match. Model a
    // future binary `mul` overload — a Number-literal fast path before the
    // Signal form.
    DispatchPattern num2 = make_pattern(
        {type_m(ParamValueType::Number), type_m(ParamValueType::Number)}, 2);
    DispatchPattern sig2 = make_pattern(
        {type_m(ParamValueType::Signal), type_m(ParamValueType::Signal)}, 2);
    std::vector<ArgDescriptor> nums = {arg_num(2.0f), arg_num(3.0f)};

    // (Number,Number) first → two Number literals bind to it.
    {
        std::vector<DispatchPattern> table = {num2, sig2};
        ResolveResult r = resolve(table, nums);
        REQUIRE(r.matched);
        CHECK(r.closest_index == 0);
        CHECK(r.pattern->params[0].type == ParamValueType::Number);
    }
    // (Signal,Signal) first → the same Numbers coerce into Signal first.
    {
        std::vector<DispatchPattern> table = {sig2, num2};
        ResolveResult r = resolve(table, nums);
        REQUIRE(r.matched);
        CHECK(r.closest_index == 0);
        CHECK(r.pattern->params[0].type == ParamValueType::Signal);
    }
}

// -----------------------------------------------------------------------------
// Phase 3 — multi-form builtin families (lookup_builtin_overloads).
// -----------------------------------------------------------------------------

TEST_CASE("builtin overloads: non-migrated names return nullptr", "[overload]") {
    // Ordinary builtins keep the single-pattern make_builtin_pattern path.
    CHECK(lookup_builtin_overloads("sine") == nullptr);
    CHECK(lookup_builtin_overloads("lp") == nullptr);
    CHECK(lookup_builtin_overloads("out") == nullptr);
    CHECK(lookup_builtin_overloads("nonexistent_fn") == nullptr);
}

TEST_CASE("builtin overloads: pan/pingpong/smooch carry one LegacyHandler pattern",
          "[overload]") {
    struct Case { std::string_view name; LegacyHandlerId id; };
    const Case cases[] = {
        {"pan",       LegacyHandlerId::Pan},
        {"pingpong",  LegacyHandlerId::Pingpong},
        {"smooch",    LegacyHandlerId::Smooch},
        {"wt",        LegacyHandlerId::Smooch},  // alias → same handler
        {"wavetable", LegacyHandlerId::Smooch},  // alias → same handler
    };
    for (const Case& c : cases) {
        const auto* patterns = lookup_builtin_overloads(c.name);
        REQUIRE(patterns != nullptr);
        REQUIRE(patterns->size() == 1);  // channel/arity branch stays in handler
        const DispatchTarget& t = (*patterns)[0].target;
        CHECK(t.kind == DispatchTarget::Kind::LegacyHandler);
        CHECK(t.legacy_handler == c.id);
    }
}

TEST_CASE("builtin overloads: delay family carries one Builtin pattern",
          "[overload]") {
    // delay/delay_ms/delay_smp keep distinct names; the time unit rides on
    // inst_rate, the pattern just routes the name through the model.
    for (std::string_view nm : {"delay", "delay_ms", "delay_smp"}) {
        const auto* patterns = lookup_builtin_overloads(nm);
        REQUIRE(patterns != nullptr);
        REQUIRE(patterns->size() == 1);
        const DispatchTarget& t = (*patterns)[0].target;
        CHECK(t.kind == DispatchTarget::Kind::Builtin);
        CHECK(t.builtin == lookup_builtin(nm));
    }
}

TEST_CASE("builtin overloads: sample family has two id-keyed patterns",
          "[overload]") {
    for (std::string_view nm : {"sample", "sample_loop"}) {
        const auto* patterns = lookup_builtin_overloads(nm);
        REQUIRE(patterns != nullptr);
        REQUIRE(patterns->size() == 2);
        // id slot (index 2): String name form first, Signal id form second.
        CHECK((*patterns)[0].params[2].kind == ArgMatcher::Kind::Type);
        CHECK((*patterns)[0].params[2].type == ParamValueType::String);
        CHECK((*patterns)[1].params[2].kind == ArgMatcher::Kind::Type);
        CHECK((*patterns)[1].params[2].type == ParamValueType::Signal);
        // Both forms emit the same SAMPLE_PLAY/LOOP builtin.
        CHECK((*patterns)[0].target.kind == DispatchTarget::Kind::Builtin);
        CHECK((*patterns)[0].target.builtin == lookup_builtin(nm));
        CHECK((*patterns)[1].target.builtin == lookup_builtin(nm));
    }
}

TEST_CASE("builtin overloads: resolve() selects the sample form by id type",
          "[overload]") {
    const auto* patterns = lookup_builtin_overloads("sample");
    REQUIRE(patterns != nullptr);
    // Mirror the codegen gate: skip args 0/1, type only the id slot (index 2).
    auto gate = [&](ValueType id) {
        std::vector<ArgDescriptor> ads(3);
        ads[0].skip = true;
        ads[1].skip = true;
        ads[2].type = id;
        return resolve(*patterns, ads);
    };
    // String id → the named (String) form, declared first.
    {
        ResolveResult r = gate(ValueType::String);
        REQUIRE(r.matched);
        CHECK(r.closest_index == 0);
        CHECK(r.pattern->params[2].type == ParamValueType::String);
    }
    // Number id → coerces into the Signal form (index 1).
    {
        ResolveResult r = gate(ValueType::Number);
        REQUIRE(r.matched);
        CHECK(r.closest_index == 1);
        CHECK(r.pattern->params[2].type == ParamValueType::Signal);
    }
    // Record / Array id → matches neither form (no coercion path to id slot).
    CHECK_FALSE(gate(ValueType::Record).matched);
    CHECK_FALSE(gate(ValueType::Array).matched);
}

// --- Codegen end-to-end: the sample gate is the first live multi-pattern
// resolve() in codegen. ---------------------------------------------------

TEST_CASE("sample codegen: String id compiles (named form)", "[overload]") {
    auto r = akkado::compile(R"(sample(1.0, 1.0, "bd") |> out(@))");
    CHECK(r.success);
    CHECK_FALSE(has_diagnostic(r, "E424"));
}

TEST_CASE("sample codegen: numeric literal id compiles (Signal form)",
          "[overload]") {
    auto r = akkado::compile(R"(sample(1.0, 1.0, 5) |> out(@))");
    CHECK(r.success);
    CHECK_FALSE(has_diagnostic(r, "E424"));
}

TEST_CASE("sample codegen: record-literal id is rejected with E424",
          "[overload]") {
    auto r = akkado::compile(R"(sample(1.0, 1.0, {a: 1}) |> out(@))");
    CHECK(has_diagnostic(r, "E424"));
}

TEST_CASE("sample codegen: array-literal id is rejected with E424",
          "[overload]") {
    auto r = akkado::compile(R"(sample(1.0, 1.0, [1, 2]) |> out(@))");
    CHECK(has_diagnostic(r, "E424"));
}

// -----------------------------------------------------------------------------
// Phase 4 — user-function overloading (codegen, end-to-end).
//
// Selection is observed via distinct builtins per overload (sine→OSC_SIN vs
// saw→OSC_SAW). `tone`/`pick` are fresh names so they don't collide with the
// stdlib `voice`/`osc`; a dedicated test exercises the stdlib-shadow path.
// -----------------------------------------------------------------------------

TEST_CASE("overload fn: selects the Number vs Pattern overload by arg type",
          "[overload]") {
    auto r = akkado::compile(R"(
        fn tone(f: Number)  -> sine(f)
        fn tone(p: Pattern) -> saw(220)
        tone(440) |> out(@)
    )");
    REQUIRE(r.success);
    // 440 is a Number → the sine overload, never the saw one.
    CHECK(has_opcode(r, cedar::Opcode::OSC_SIN));
    CHECK_FALSE(has_opcode(r, cedar::Opcode::OSC_SAW));
}

TEST_CASE("overload fn: a pattern argument selects the Pattern overload",
          "[overload]") {
    auto r = akkado::compile(R"(
        fn tone(f: Number)  -> sine(f)
        fn tone(p: Pattern) -> saw(220)
        n"c4 e4" |> tone(@) |> out(@)
    )");
    REQUIRE(r.success);
    // A polyphonic pattern can't bind the Number overload → the saw form.
    CHECK(has_opcode(r, cedar::Opcode::OSC_SAW));
    CHECK_FALSE(has_opcode(r, cedar::Opcode::OSC_SIN));
}

TEST_CASE("overload polyphonic mirror: a scalar-incompatible pattern skips "
          "Signal/Any and reaches a Pattern overload", "[overload]") {
    // Resolver-level: a polyphonic non-sample Pattern (binding would E160 it in
    // a scalar slot) must NOT match Type{Signal} or Any, but must match
    // Type{Pattern} — so a Pattern overload wins even when Signal is declared
    // first. (End-to-end coverage lives in the binding's E160 path.)
    ArgDescriptor poly;
    poly.type = ValueType::Pattern;
    poly.polyphonic_scalar_incompatible = true;

    CHECK_FALSE(matches_arg(type_m(ParamValueType::Signal), poly));
    CHECK_FALSE(matches_arg(any_m(), poly));
    CHECK(matches_arg(type_m(ParamValueType::Pattern), poly));

    std::vector<DispatchPattern> patterns = {
        make_pattern({type_m(ParamValueType::Signal)}, 1),   // declared first
        make_pattern({type_m(ParamValueType::Pattern)}, 1),
    };
    auto r = resolve(patterns, {poly});
    REQUIRE(r.matched);
    CHECK(r.closest_index == 1);  // the Pattern overload, not the Signal one
}

TEST_CASE("overload fn: no matching overload warns (W170) and falls back",
          "[overload]") {
    // A String arg matches neither the Number nor the Pattern overload → W170
    // + first overload, whose binding then emits its own precise E184.
    auto r = akkado::compile(R"(
        fn tone(f: Number)  -> sine(f)
        fn tone(p: Pattern) -> saw(220)
        tone("hi") |> out(@)
    )");
    CHECK(has_diagnostic(r, "W170"));
}

TEST_CASE("overload fn: `_` partial application warns (W170) and falls back",
          "[overload]") {
    auto r = akkado::compile(R"(
        fn tone(f: Number)  -> sine(f)
        fn tone(p: Pattern) -> saw(220)
        f = tone(_)
        f(440) |> out(@)
    )");
    CHECK(has_diagnostic(r, "W170"));
}

TEST_CASE("overload fn: single definition does not warn", "[overload]") {
    // The size==1 fast path must never emit the overload warning.
    auto r = akkado::compile(R"(
        fn tone(f: Number) -> sine(f)
        tone(440) |> out(@)
    )");
    REQUIRE(r.success);
    CHECK_FALSE(has_diagnostic(r, "W170"));
}

TEST_CASE("overload fn: arity dispatch picks the matching overload",
          "[overload]") {
    auto r = akkado::compile(R"(
        fn tone(a)    -> sine(a)
        fn tone(a, b) -> saw(a + b)
        tone(220, 220) |> out(@)
    )");
    REQUIRE(r.success);
    CHECK(has_opcode(r, cedar::Opcode::OSC_SAW));
    CHECK_FALSE(has_opcode(r, cedar::Opcode::OSC_SIN));
}

TEST_CASE("overload fn: user definition shadows the stdlib base layer",
          "[overload]") {
    // The stdlib defines `fn voice(events: Stream, intervals)`. A user `voice`
    // with a different signature must SHADOW it (not accumulate), so passing
    // `voice` as an instrument resolves to the user body, not the stdlib one.
    auto r = akkado::compile(R"(
        fn voice(f, g, v, e) -> saw(f)
        unison(220, 1, 1, voice) |> out(@)
    )");
    REQUIRE(r.success);
    CHECK_FALSE(has_diagnostic(r, "E242"));
    CHECK(has_opcode(r, cedar::Opcode::OSC_SAW));
}

TEST_CASE("overload fn: shadow stdlib then accumulate a user overload",
          "[overload]") {
    // First user `voice` shadows the stdlib; a second distinct-signature user
    // `voice` accumulates with the first (the Phase 4 flagship shape).
    auto r = akkado::compile(R"(
        fn voice(f: Number)  -> sine(f)
        fn voice(p: Pattern) -> saw(220)
        voice(440) |> out(@)
    )");
    REQUIRE(r.success);
    CHECK(has_opcode(r, cedar::Opcode::OSC_SIN));
    CHECK_FALSE(has_opcode(r, cedar::Opcode::OSC_SAW));
}

TEST_CASE("overload fn: shared-block overloads do not collide",
          "[overload]") {
    // Two shareable same-name overloads (no defaults, each called >=2x) with
    // distinct bodies must keep distinct compiled blocks — a name-keyed cache
    // would route one overload's calls into the other's body.
    auto r = akkado::compile(R"(
        fn tone(a)    -> sine(a)
        fn tone(a, b) -> saw(a + b)
        s = tone(110) + tone(220) + tone(110, 5) + tone(220, 5)
        s |> out(@)
    )");
    REQUIRE(r.success);
    CHECK(has_opcode(r, cedar::Opcode::OSC_SIN));   // 1-arg body
    CHECK(has_opcode(r, cedar::Opcode::OSC_SAW));   // 2-arg body
}

TEST_CASE("overload fn: redefining the same signature replaces (no extra body)",
          "[overload]") {
    auto r = akkado::compile(R"(
        fn tone(f: Number) -> sine(f)
        fn tone(g: Number) -> saw(g)
        tone(440) |> out(@)
    )");
    REQUIRE(r.success);
    // Same signature → the saw redefinition replaces the sine one.
    CHECK(has_opcode(r, cedar::Opcode::OSC_SAW));
    CHECK_FALSE(has_opcode(r, cedar::Opcode::OSC_SIN));
}
