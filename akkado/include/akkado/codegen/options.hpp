#pragma once

// Shared record-as-options helper (PRD prd-records-system-unification §5.5).
// Generalizes the per-viz extract_options_json by validating field names
// against the builtin's declared OptionSchema. Recognized fields land on
// `payload.fields`; unrecognized field names land on `payload.unknown_fields`
// and are dropped from the emitted JSON. The unknown_fields list reserves
// future room for a W160 warning pass once the spread PRD ships W160 infra.

#include "akkado/ast.hpp"
#include "akkado/builtins.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace akkado {
namespace codegen {

struct OptionValue {
    enum class Kind { Number, String, Bool };
    Kind        kind = Kind::Number;
    double      num  = 0.0;
    std::string str;
    bool        b    = false;
};

struct OptionsPayload {
    // Recognized fields the user typed, in source order.
    // string_view points into the AST arena; payload lifetime is bounded by the AST.
    std::vector<std::pair<std::string_view, OptionValue>> fields;

    // Field names the user typed that are NOT in the schema. Dropped from
    // to_json(); reserved for a future W160 warning pass.
    std::vector<std::string_view> unknown_fields;

    [[nodiscard]] std::optional<double>           get_number(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> get_string(std::string_view name) const;
    [[nodiscard]] std::optional<bool>             get_bool  (std::string_view name) const;

    // Compact JSON, locale-free. Recognized fields only, in source order.
    // Returns "" when no recognized fields were captured (matches the
    // pre-Phase-5 contract relied on by the web UI).
    [[nodiscard]] std::string to_json() const;

    [[nodiscard]] bool empty() const { return fields.empty(); }
};

// Walk a record-literal arg node, validate field names against `schema`,
// classify each into `fields` (recognized) or `unknown_fields`. Returns an
// empty payload when arg_node is NULL_NODE or the underlying value is not
// a RecordLit. Field-value type mismatches (e.g. schema says Number, user
// passed a String) are accepted as-is — the JSON consumer historically
// tolerated mixed types and the diagnostic for that is a future concern.
OptionsPayload extract_options(const AstArena&     arena,
                               NodeIndex           arg_node,
                               const OptionSchema& schema);

}  // namespace codegen
}  // namespace akkado
