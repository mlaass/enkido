#pragma once

// Akkado overload-resolution model + resolver.
//
// PRD: docs/prd-builtin-overload-resolution.md
//
// This is the Phase-1 foundation: a single declarative dispatch model that will
// eventually unify builtin overloads, operators (op_*), and user-function
// overloading. Phase 1 ships the data model + resolver and wires codegen to
// resolve *single-pattern* builtins (each builtin = one pattern synthesized from
// its `param_types`/`defaults`). It is a no-behavior-change refactor of the old
// inline `param_types` type-check loop.
//
// Resolution model (PRD §5):
//   - Every overloadable name owns an ordered list of DispatchPatterns.
//   - A pattern is an ordered list of per-argument ArgMatchers.
//   - Resolution is first-match in declaration order; coercion (via
//     `type_compatible`) counts as a match, so ordering is the only knob.
//   - No match (even with coercion) is an error naming the closest candidate.
//
// Phase 1 only exercises the Type/Any matcher kinds via single-pattern builtins.
// The literal-value kinds (StringLiteral/NumberLiteral), `matches()`/`resolve()`
// multi-pattern resolution, and `closest_candidate()` are implemented and
// unit-tested here, but not driven by codegen until Phases 2/3.

#include "akkado/typed_value.hpp"
#include "akkado/builtins.hpp"  // BuiltinInfo, type_compatible, MAX_BUILTIN_PARAMS

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace akkado {

/// A single argument matcher (one parameter slot of a DispatchPattern).
struct ArgMatcher {
    enum class Kind : std::uint8_t {
        Type,           // matched via type_compatible() against `type`
        StringLiteral,  // matches a compile-time String literal == string_id
        NumberLiteral,  // matches a compile-time Number literal == number
        Any,            // matches anything (see reject_uncoercible_signal)
    };

    Kind kind = Kind::Any;

    // Kind::Type — the expected parameter type. Stored as ParamValueType (NOT
    // ValueType) on purpose: `type_compatible`'s expected-side argument and the
    // legacy E160 message text (`param_value_type_name`) both take a
    // ParamValueType, so this keeps the match and the diagnostic byte-identical
    // with zero conversions. (PRD §5.1 sketches `ValueType type`; this is the
    // faithful refinement.)
    ParamValueType type = ParamValueType::Any;

    // Kind::StringLiteral — interned literal id (FNV-1a). Wired in Phase 3.
    std::uint32_t string_id = 0;

    // Kind::NumberLiteral — literal value. Wired in Phase 3.
    float number = 0.0f;

    // Phase-1 migration bridge, meaningful ONLY when kind == Any. Mirrors the
    // legacy IMPLICIT signal-slot rule: when a builtin coerces args to signal
    // (`args_are_signal`) and this is a real parameter slot, reject
    // Function/StateCell ("no coercion path") while every other type passes.
    // This slot is strictly MORE permissive than a Type{Signal} matcher (it
    // accepts String/Array/Record/Number/Pattern), which is why it cannot be a
    // Kind::Type. Removed in Phase 2, when these slots become explicit Signal
    // patterns.
    bool reject_uncoercible_signal = false;
};

/// Where a matched pattern dispatches to. Phase 1 only uses Builtin and codegen
/// still emits directly from the BuiltinInfo it already holds, so this is
/// forward-compatible scaffolding rather than a consumed emission target.
struct DispatchTarget {
    enum class Kind : std::uint8_t { Builtin, UserFunction, LegacyHandler };
    Kind kind = Kind::Builtin;
    const BuiltinInfo* builtin = nullptr;  // Kind::Builtin
    // Phase 4 adds a user-fn body NodeIndex; Phase 3/5 add a handler id.
};

/// One overload form: an ordered list of per-argument matchers plus the number
/// of leading required slots (trailing slots are optional / defaulted).
struct DispatchPattern {
    std::vector<ArgMatcher> params;   // for builtins, sized to MAX_BUILTIN_PARAMS
    std::uint8_t required_count = 0;
    DispatchTarget target;
};

/// Registry: name -> ordered pattern list. Declared for Phase 2+ (operators /
/// user-fn overloading); Phase 1 synthesizes single patterns on demand and does
/// not populate a persistent table.
using OverloadTable =
    std::unordered_map<std::string_view, std::vector<DispatchPattern>>;

/// Resolution input — one descriptor per provided argument slot.
struct ArgDescriptor {
    ValueType type = ValueType::Void;
    bool skip = false;  // codegen-skipped slots (underscore/error/Void/DynArray)

    // Literal info — populated in Phase 3 so StringLiteral/NumberLiteral
    // matchers can fire. Phase 1 leaves these defaulted.
    bool is_string_literal = false;
    std::uint32_t string_id = 0;
    bool is_number_literal = false;
    float number = 0.0f;
};

/// Per-slot match failure. Lets a caller emit one diagnostic per failing slot,
/// at that slot's source location, with the correct message variant.
struct SlotFailure {
    std::size_t index = 0;
    ArgMatcher::Kind matcher_kind = ArgMatcher::Kind::Any;
    ParamValueType expected = ParamValueType::Any;  // for Kind::Type message
    bool signal_coerce_reject = false;              // true -> IMPLICIT message
};

/// Result of resolving a call against a pattern list.
struct ResolveResult {
    bool matched = false;
    const DispatchPattern* pattern = nullptr;  // matched, or closest on failure
    std::vector<SlotFailure> failures;         // empty when matched
    std::size_t closest_index = 0;             // index into the pattern list
};

// --- Free functions ---------------------------------------------------------

/// Phase-1 single-pattern synthesis from a BuiltinInfo. The result mirrors the
/// builtin's `param_types` (explicit Type matchers), `args_are_signal` +
/// `total_params()` (implicit signal-coerce Any matchers), and `input_count`
/// (required_count). `params` has length MAX_BUILTIN_PARAMS.
DispatchPattern make_builtin_pattern(const BuiltinInfo& info);

/// Per-slot match predicate — the primitive Phase-1 codegen uses.
bool matches_arg(const ArgMatcher& matcher, const ArgDescriptor& arg);

/// Whole-pattern match (arity-aware, PRD §5.2). When `out_failures` is non-null
/// it collects every failing slot (max-error semantics, used for diagnostics
/// and closest-candidate ranking). NOTE: arity-aware — used by resolve()/unit
/// tests, NOT by the Phase-1 codegen path (which must not add arity gating).
bool matches(const DispatchPattern& pattern,
             const std::vector<ArgDescriptor>& args,
             std::vector<SlotFailure>* out_failures = nullptr);

/// First-match resolution + closest-candidate on failure (PRD §5.2/§5.3).
ResolveResult resolve(const std::vector<DispatchPattern>& patterns,
                      const std::vector<ArgDescriptor>& args);

/// Index of the closest no-match candidate: fewest slots failing even with
/// coercion, then smallest arity distance; ties resolve to earliest
/// declaration (PRD §5.3). Returns 0 for an empty pattern list.
std::size_t closest_candidate(const std::vector<DispatchPattern>& patterns,
                              const std::vector<ArgDescriptor>& args);

}  // namespace akkado
