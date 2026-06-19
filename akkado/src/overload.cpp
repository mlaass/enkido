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

const std::vector<DispatchPattern>* lookup_operator_overloads(std::string_view name) {
    // Lazily-built registry of operator overloads, keyed by the builtin name the
    // parser desugars each operator to (parser.cpp parse_binary). Single pattern
    // per operator in Phase 2 — every pattern mirrors make_builtin_pattern of the
    // underlying builtin, so the matchers stay identical to the generic per-arg
    // type-check path; only the DispatchTarget differs.
    static const OverloadTable table = [] {
        OverloadTable t;

        // Arithmetic (+ - * / ^): the array/stereo broadcasting handler
        // (handle_binary_op_call) is the emission body, selected via a
        // LegacyHandler target (PRD §5.6 migration bridge).
        static constexpr std::string_view kArithmetic[] = {
            "add", "sub", "mul", "div", "pow",
        };
        for (std::string_view nm : kArithmetic) {
            const BuiltinInfo* info = lookup_builtin(nm);
            if (!info) continue;  // every operator name is a builtin; guard defensively
            DispatchPattern p = make_builtin_pattern(*info);
            p.target.kind = DispatchTarget::Kind::LegacyHandler;
            p.target.legacy_handler = LegacyHandlerId::BinaryOpBroadcast;
            t[nm].push_back(std::move(p));
        }

        // Comparison (== != < <= > >=) and logical (&& || !): emit the opcode
        // directly through the generic builtin path, so a Builtin target (already
        // set by make_builtin_pattern) is correct.
        static constexpr std::string_view kBuiltinOps[] = {
            "eq", "neq", "lt", "gt", "lte", "gte",  // comparison
            "band", "bor", "bnot",                  // logical (bnot is unary)
        };
        for (std::string_view nm : kBuiltinOps) {
            const BuiltinInfo* info = lookup_builtin(nm);
            if (!info) continue;
            t[nm].push_back(make_builtin_pattern(*info));
        }

        return t;
    }();

    auto it = table.find(name);
    return it != table.end() ? &it->second : nullptr;
}

const std::vector<DispatchPattern>* lookup_builtin_overloads(std::string_view name) {
    // Lazily-built registry of the multi-form builtin families migrated in
    // Phase 3 (PRD §8). Names NOT in this table keep the single-pattern
    // make_builtin_pattern path in codegen — no behavior change for the ~200
    // ordinary builtins.
    static const OverloadTable table = [] {
        OverloadTable t;

        // --- pan / pingpong / smooch: one LegacyHandler pattern each. The real
        // dispatch dimension (channel width for pan/pingpong, arg count for
        // smooch) is orthogonal to the type model (PRD §3) and stays inside the
        // existing handler; the pattern only routes the name declaratively,
        // mirroring Phase 2's arithmetic-operator migration.
        struct LegacyEntry {
            std::string_view name;
            LegacyHandlerId  handler;
        };
        static constexpr LegacyEntry kLegacy[] = {
            {"pan",       LegacyHandlerId::Pan},
            {"pingpong",  LegacyHandlerId::Pingpong},
            {"smooch",    LegacyHandlerId::Smooch},
            {"wt",        LegacyHandlerId::Smooch},
            {"wavetable", LegacyHandlerId::Smooch},
        };
        for (const LegacyEntry& e : kLegacy) {
            const BuiltinInfo* info = lookup_builtin(e.name);
            if (!info) continue;  // defensive: every name above is a builtin
            DispatchPattern p = make_builtin_pattern(*info);
            p.target.kind = DispatchTarget::Kind::LegacyHandler;
            p.target.legacy_handler = e.handler;
            t[e.name].push_back(std::move(p));
        }

        // --- delay / delay_ms / delay_smp: one Builtin pattern each. Emission
        // falls through to the generic path; the time unit comes from
        // BuiltinInfo::inst_rate (the three entries declare 0/1/2), not a
        // func-name string compare in codegen.
        static constexpr std::string_view kDelay[] = {
            "delay", "delay_ms", "delay_smp",
        };
        for (std::string_view nm : kDelay) {
            const BuiltinInfo* info = lookup_builtin(nm);
            if (!info) continue;
            t[nm].push_back(make_builtin_pattern(*info));
        }

        // --- sample / sample_loop: TWO Builtin patterns keyed by the id slot
        // (index 2) type. Declaration order is dispatch priority: the String
        // (sample-name) form first, the Signal (numeric / runtime id) form
        // second. type_compatible coerces Number→Signal, so a numeric literal
        // id binds to the Signal form; an Array/Record id matches neither and
        // resolve() reports E424. Both forms emit through the generic
        // SAMPLE_PLAY path — resolve() selects the form and gates the id type.
        static constexpr std::string_view kSample[] = {
            "sample", "sample_loop",
        };
        for (std::string_view nm : kSample) {
            const BuiltinInfo* info = lookup_builtin(nm);
            if (!info) continue;

            DispatchPattern named = make_builtin_pattern(*info);
            named.params[2].kind = ArgMatcher::Kind::Type;
            named.params[2].type = ParamValueType::String;
            named.params[2].reject_uncoercible_signal = false;

            DispatchPattern numeric = make_builtin_pattern(*info);
            numeric.params[2].kind = ArgMatcher::Kind::Type;
            numeric.params[2].type = ParamValueType::Signal;
            numeric.params[2].reject_uncoercible_signal = false;

            t[nm].push_back(std::move(named));
            t[nm].push_back(std::move(numeric));
        }

        return t;
    }();

    auto it = table.find(name);
    return it != table.end() ? &it->second : nullptr;
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
