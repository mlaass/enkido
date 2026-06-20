#pragma once

// Akkado overload-resolution model + resolver.
//
// PRD: docs/prd-builtin-overload-resolution.md
//
// This is the foundation: a single declarative dispatch model that will
// eventually unify builtin overloads, operators, and user-function overloading.
// Phase 1 shipped the data model + resolver and wires codegen to resolve
// *single-pattern* builtins (each builtin = one pattern synthesized from its
// `param_types`/`defaults`). It is a no-behavior-change refactor of the old
// inline `param_types` type-check loop.
//
// Phase 2 brings operators into the model: they reuse their existing builtin
// names (`add`/`mul`/`eq`/`band`/…, what the parser already desugars to — no
// `op_*` names) as keys into a shared operator OverloadTable. The matched
// pattern's DispatchTarget selects the emission body — arithmetic carries a
// LegacyHandler target (the array/stereo broadcasting handler), comparison/
// logical a Builtin target. See `lookup_operator_overloads`.
//
// Resolution model (PRD §5):
//   - Every overloadable name owns an ordered list of DispatchPatterns.
//   - A pattern is an ordered list of per-argument ArgMatchers.
//   - Resolution is first-match in declaration order; coercion (via
//     `type_compatible`) counts as a match, so ordering is the only knob.
//   - No match (even with coercion) is an error naming the closest candidate.
//
// `matches()`/`resolve()`/`closest_candidate()` are implemented and unit-tested
// here. Phase 3 brings the multi-form builtin families into the model via
// `lookup_builtin_overloads`: pan/pingpong/smooch/delay* migrate as single
// declarative patterns (like Phase 2's operators), and `sample`/`sample_loop`
// become the first family to drive codegen through a live multi-pattern
// `resolve()` — selecting the String-name vs Number-id form by argument type.
// (The PRD §4.1 `delay(sig,"ms",t)` literal-unit form was descoped; delay keeps
// its three names, so the StringLiteral/NumberLiteral matchers remain
// resolver-only with no codegen consumer yet.)

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
    // Kind::Type. (The Phase-1 design sketched retiring this in Phase 2 by
    // making operator slots explicit Signal patterns; that migration is deferred
    // — Phase 2 reuses the existing synthesized matchers verbatim to keep the
    // generic per-arg E160 path byte-identical.)
    bool reject_uncoercible_signal = false;
};

/// Identifies which bespoke codegen handler a LegacyHandler target dispatches to
/// (the PRD §5.6 migration bridge: a declarative pattern that selects existing
/// hand-written codegen instead of inlining an opcode). Phase 2 added the
/// arithmetic-operator broadcasting handler; Phase 3 migrates the multi-form
/// stereo/wavetable handlers whose dispatch dimension (channel width / arg count)
/// is orthogonal to the type model and therefore stays inside the handler; Phase 5
/// migrates the heavy pattern / higher-order handlers (poly/each/transport/midi),
/// whose dispatch dimension (arity / mode / options) likewise stays inside.
enum class LegacyHandlerId : std::uint8_t {
    None,
    BinaryOpBroadcast,  // handle_binary_op_call — +,-,*,/,^ array/stereo broadcast
    Pan,                // handle_pan_call — mono PAN vs stereo PAN_STEREO (channel width)
    Pingpong,           // handle_pingpong_call — stereo vs explicit-L/R forms
    Smooch,             // handle_smooch_call — wavetable smooch/wt/wavetable, 2-4 args
    // Phase 5: heavy pattern / higher-order handlers. Single-pattern routing;
    // the dispatch dimension (arity / mode / options) stays inside each handler.
    Poly,               // handle_poly_call — poly / legato voice allocation
    Mono,               // handle_mono_call — mono downmix
    Each,               // handle_each_call — each(pattern, lambda)
    EachVoice,          // handle_each_voice_call — each_voice(pattern, lambda)
    Transport,          // handle_transport_call — transport sequencer
    Midi,               // handle_midi_call — runtime MIDI event source
};

/// Where a matched pattern dispatches to. Phase 2 consumes Builtin (emit the
/// opcode) and LegacyHandler (run an existing bespoke handler) as live dispatch
/// targets for operators; UserFunction is Phase-4 scaffolding.
struct DispatchTarget {
    enum class Kind : std::uint8_t { Builtin, UserFunction, LegacyHandler };
    Kind kind = Kind::Builtin;
    const BuiltinInfo* builtin = nullptr;            // Kind::Builtin
    LegacyHandlerId legacy_handler = LegacyHandlerId::None;  // Kind::LegacyHandler
    // Phase 4 adds a user-fn body NodeIndex.
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

    // Phase 4 user-fn overloading: set when the argument is a *polyphonic*
    // non-sample Pattern. The user-fn binding rejects such a value in a scalar
    // slot (`: Signal` or un-annotated) with E160, so a `Kind::Type{Signal}` or
    // `Kind::Any` matcher must NOT select it — otherwise resolve() would pick
    // an overload that then fails binding, skipping a `: Pattern` overload the
    // user wanted. This bit makes selection mirror the binding rule exactly.
    // (Builtin/operator resolution never sets it, so their behavior is
    // unchanged.)
    bool polyphonic_scalar_incompatible = false;
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

/// Phase-2 operator dispatch. Returns the ordered pattern list for an operator
/// name (`add`/`sub`/`mul`/`div`/`pow`, `eq`/`neq`/`lt`/`gt`/`lte`/`gte`,
/// `band`/`bor`/`bnot` — what the parser desugars `+ - * / ^`, `== != < <= > >=`,
/// `&& || !` to), or nullptr if `name` is not an operator. Each operator has a
/// single pattern in Phase 2: arithmetic carries a LegacyHandler/BinaryOpBroadcast
/// target, comparison/logical a Builtin target. The patterns mirror
/// `make_builtin_pattern` of the underlying builtin, so the matchers are
/// identical to the generic per-arg type-check path.
const std::vector<DispatchPattern>* lookup_operator_overloads(std::string_view name);

/// Phase-3 multi-form builtin dispatch. Returns the ordered pattern list for a
/// migrated multi-form builtin name, or nullptr for any other name (which keeps
/// the single-pattern `make_builtin_pattern` path unchanged). Families:
///   - `pan`, `pingpong`, `smooch`/`wt`/`wavetable` — one LegacyHandler pattern
///     each (Pan/Pingpong/Smooch). Their dispatch dimension (channel width /
///     arg count) is NOT a type-model dimension (PRD §3), so it stays inside the
///     existing handler; the pattern just routes the name declaratively.
///   - `delay`/`delay_ms`/`delay_smp` — one Builtin pattern each; emission falls
///     through to the generic path and the time unit comes from `inst_rate`.
///   - `sample`/`sample_loop` — TWO Builtin patterns keyed by the id slot type
///     (`String` name form first, `Number` id form second). This is the first
///     family that drives codegen through a live `resolve()` type dispatch, with
///     a no-match diagnostic (E424) when the id slot is neither String nor Number.
const std::vector<DispatchPattern>* lookup_builtin_overloads(std::string_view name);

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
