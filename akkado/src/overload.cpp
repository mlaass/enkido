#include "akkado/overload.hpp"

#include <algorithm>

// PRD: docs/prd-builtin-overload-resolution.md
//
// Phase-1 implementation of the overload model. `make_builtin_pattern` +
// `matches_arg` are what codegen drives today; `matches`/`resolve`/
// `closest_candidate` are the multi-pattern machinery for Phases 2/3, built and
// unit-tested now so later phases only need to wire them in.

namespace akkado {

DispatchPattern make_builtin_pattern(const BuiltinInfo& info) {
    DispatchPattern p;
    p.params.resize(MAX_BUILTIN_PARAMS);

    const std::size_t total = info.total_params();
    for (std::size_t i = 0; i < MAX_BUILTIN_PARAMS; ++i) {
        ArgMatcher& m = p.params[i];
        const ParamValueType pt = info.param_types[i];
        if (pt != ParamValueType::Any) {
            // EXPLICIT annotation — strict type_compatible match.
            m.kind = ArgMatcher::Kind::Type;
            m.type = pt;
        } else {
            // IMPLICIT signal-coerce slot, or a pure-Any slot beyond the
            // builtin's parameters. The legacy `arg_idx < total_params()` guard
            // is folded into the flag here so matches_arg needs no slot index.
            m.kind = ArgMatcher::Kind::Any;
            m.reject_uncoercible_signal = info.args_are_signal && i < total;
        }
    }

    p.required_count = info.input_count;
    p.target.kind = DispatchTarget::Kind::Builtin;
    p.target.builtin = &info;
    return p;
}

bool matches_arg(const ArgMatcher& matcher, const ArgDescriptor& arg) {
    switch (matcher.kind) {
        case ArgMatcher::Kind::Type:
            // Coercion counts as a match (PRD §5.2).
            return type_compatible(arg.type, matcher.type);

        case ArgMatcher::Kind::StringLiteral:
            // Literal guards never match non-literals (PRD §9 edge case 4).
            return arg.is_string_literal && arg.string_id == matcher.string_id;

        case ArgMatcher::Kind::NumberLiteral:
            return arg.is_number_literal && arg.number == matcher.number;

        case ArgMatcher::Kind::Any:
            // The IMPLICIT signal-coerce rule: only Function/StateCell have no
            // audio meaning at a coerced signal slot; everything else passes.
            if (matcher.reject_uncoercible_signal &&
                (arg.type == ValueType::Function ||
                 arg.type == ValueType::StateCell)) {
                return false;
            }
            return true;
    }
    return false;
}

bool matches(const DispatchPattern& pattern,
             const std::vector<ArgDescriptor>& args,
             std::vector<SlotFailure>* out_failures) {
    // Arity gating (PRD §5.2). Note: this is why matches() is kept OUT of the
    // Phase-1 codegen path — the legacy generic path does no arity gating.
    if (args.size() < pattern.required_count) return false;
    if (args.size() > pattern.params.size()) return false;

    bool ok = true;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].skip) continue;
        if (matches_arg(pattern.params[i], args[i])) continue;

        ok = false;
        if (out_failures) {
            const ArgMatcher& m = pattern.params[i];
            SlotFailure f;
            f.index = i;
            f.matcher_kind = m.kind;
            f.expected = m.type;
            f.signal_coerce_reject =
                m.kind == ArgMatcher::Kind::Any && m.reject_uncoercible_signal;
            out_failures->push_back(f);
        } else {
            // No failure collection requested — stop at the first mismatch.
            return false;
        }
    }
    return ok;
}

std::size_t closest_candidate(const std::vector<DispatchPattern>& patterns,
                              const std::vector<ArgDescriptor>& args) {
    if (patterns.empty()) return 0;

    std::size_t best_index = 0;
    std::size_t best_fail = static_cast<std::size_t>(-1);
    std::size_t best_arity = static_cast<std::size_t>(-1);

    for (std::size_t pi = 0; pi < patterns.size(); ++pi) {
        const DispatchPattern& p = patterns[pi];

        // Count slots that fail even with coercion, over the overlap.
        std::size_t fail = 0;
        const std::size_t overlap = std::min(args.size(), p.params.size());
        for (std::size_t i = 0; i < overlap; ++i) {
            if (args[i].skip) continue;
            if (!matches_arg(p.params[i], args[i])) ++fail;
        }

        // Distance of the call's arity from the pattern's acceptable range
        // [required_count, params.size()].
        std::size_t arity = 0;
        if (args.size() < p.required_count) {
            arity = p.required_count - args.size();
        } else if (args.size() > p.params.size()) {
            arity = args.size() - p.params.size();
        }

        // Rank by failing slots, then arity distance; ties keep the earliest.
        if (fail < best_fail || (fail == best_fail && arity < best_arity)) {
            best_fail = fail;
            best_arity = arity;
            best_index = pi;
        }
    }

    return best_index;
}

ResolveResult resolve(const std::vector<DispatchPattern>& patterns,
                      const std::vector<ArgDescriptor>& args) {
    ResolveResult result;

    for (std::size_t i = 0; i < patterns.size(); ++i) {
        if (matches(patterns[i], args, nullptr)) {
            result.matched = true;
            result.pattern = &patterns[i];
            result.closest_index = i;
            return result;
        }
    }

    // No match — report the closest candidate and its failing slots.
    if (!patterns.empty()) {
        const std::size_t closest = closest_candidate(patterns, args);
        result.closest_index = closest;
        result.pattern = &patterns[closest];
        matches(patterns[closest], args, &result.failures);
    }
    return result;
}

}  // namespace akkado
