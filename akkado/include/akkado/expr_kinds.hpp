#pragma once

// Hardening PRD Phase 7 (PRD-13): single source of truth for
// expression-kind predicates and the MIDI→Hz conversion. Replaces the
// per-file near-copies in analyzer.cpp, shape_index.cpp,
// codegen_functions.cpp and const_eval.cpp.

#include <cmath>
#include <optional>
#include <string_view>

#include "ast.hpp"
#include "string_interner.hpp"
#include "symbol_table.hpp"  // ConstValue, SymbolTable

namespace akkado::expr_kinds {

/// MIDI note number to Hz. Single source of truth.
inline double midi_to_hz(double midi) {
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

/// Canonical list of pattern-producing builtin call names. `chord(...)`
/// and `timeline(...)` are the live pattern-producing call forms; "seq"
/// is retained from the analyzer's historical list (the bare `seq(...)`
/// call no longer resolves, so the entry is inert on successful
/// compiles). `pat`/`value`/`note` call forms were removed by
/// prd-remove-pat-builtin (2026-05-20); `sample(...)` is the
/// SAMPLE_PLAY player, not a pattern producer.
constexpr bool is_pattern_producing_call_name(std::string_view name) {
    return name == "chord" || name == "seq" || name == "timeline";
}

/// True if the expression is a literal value (Number, Bool, String,
/// Pitch). No side effects, no lookup needed.
inline bool is_literal_value(const Node& n) {
    switch (n.type) {
        case NodeType::NumberLit:
        case NodeType::BoolLit:
        case NodeType::StringLit:
        case NodeType::PitchLit:
            return true;
        default:
            return false;
    }
}

/// Structural pattern-producer check: MiniLiteral literals,
/// pattern-producing builtin calls, and method-call chains rooted at
/// either. Purely syntactic — callers that can resolve Identifier
/// references against a symbol table (the analyzer) layer that on top.
bool is_pattern_producer(const AstArena& arena, NodeIndex node,
                         const StringInterner& interner);

/// Conservative compile-time constant resolve: Number / Bool / Pitch
/// literals, const-symbol identifiers, and arrays thereof. Returns
/// nullopt for anything else — deliberately narrower than the full
/// ConstEvaluator (no arithmetic folding), because call sites use it as
/// an "is this trivially const?" gate with zero diagnostic side effects.
std::optional<ConstValue> try_const_value(const AstArena& arena,
                                          NodeIndex node,
                                          const SymbolTable& symbols);

/// True if try_const_value() would succeed.
inline bool is_const_evaluable(const AstArena& arena, NodeIndex node,
                               const SymbolTable& symbols) {
    return try_const_value(arena, node, symbols).has_value();
}

}  // namespace akkado::expr_kinds
