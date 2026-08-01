#pragma once

// Hardening PRD Phase 7 (PRD-13): single canonical named-argument slot
// resolver. The three former near-mirrors — the analyzer's BuiltinInfo
// and param-name-vector reorder overloads and codegen's spread-call
// variant — build their inputs, call assign_named_arg_slots(), then
// materialise the result in their own representation (AST child-list
// relink vs ExpandedArg vector).

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "diagnostics.hpp"

namespace akkado {

/// One call argument as seen by the named-arg slot resolver.
struct NamedArgInput {
    std::optional<std::string> name;  // nullopt = positional
    SourceLocation loc;               // anchors diagnostics
};

/// find_param sentinel: abort the resolve without a core diagnostic —
/// the callback already emitted its own (e.g. E263 open-kwarg overflow).
inline constexpr int kNamedArgAbort = -2;

/// Result of assign_named_arg_slots().
struct NamedArgSlots {
    bool ok = true;           // false → caller aborts the call
    bool has_named = false;   // false → nothing to reorder (slot_source empty)
    /// slot → index into the input args; -1 = gap (caller fills with an
    /// underscore placeholder / declared default). Empty when has_named
    /// is false, or when every argument was dropped.
    std::vector<int> slot_source;
    /// Input indices dropped as unknown names (drop_unknown mode only) —
    /// caller emits its W-class warning per entry.
    std::vector<std::size_t> dropped;
    /// Diagnostic payload when ok == false (code == nullptr when the
    /// find_param callback already emitted its own).
    const char* code = nullptr;   // "E009" / "E010" / "E012"
    std::string message;
    SourceLocation error_loc{};
};

/// Resolve each argument to a canonical parameter slot with the shared
/// duplicate (E010), positional-after-named (E009) and positional/named
/// collision (E012) checks.
///
/// `find_param(name, arg_idx)` maps a parameter name to its slot index;
/// negative = unknown (kNamedArgAbort = abort, diagnostic already
/// emitted by the callback). Unknown names are a hard error unless
/// `drop_unknown` is set, in which case they land in `dropped`.
NamedArgSlots assign_named_arg_slots(
    const std::vector<NamedArgInput>& args,
    const std::string& func_name,
    const std::function<int(std::string_view, std::size_t)>& find_param,
    bool drop_unknown);

}  // namespace akkado
