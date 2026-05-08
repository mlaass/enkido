// Implementation of the shared record-as-options helper.
// PRD prd-records-system-unification §5.5 / Phase 5 deliverable.
//
// Walks a RecordLit arg node, splits fields into recognized vs unknown by
// looking up names in the builtin's declared OptionSchema, and serializes
// the recognized half to a compact JSON string compatible with the
// pre-Phase-5 extract_options_json output.

#include "akkado/codegen/options.hpp"

#include <cstdio>   // snprintf — locale-free float formatting (AudioWorklet has no <locale>)
#include <cstring>

namespace akkado {
namespace codegen {

namespace {

// True when `schema` declares a field with the given name.
bool schema_has_field(const OptionSchema& schema, std::string_view name) {
    for (std::uint8_t i = 0; i < schema.field_count; ++i) {
        if (schema.fields[i].name == name) return true;
    }
    return false;
}

// Append a JSON-escaped string value. The viz schema only declares plain
// identifiers and short string defaults today, so bare-bones escaping is
// enough — match the pre-Phase-5 helper's behaviour (which did not escape
// at all). Backslashes and double quotes are escaped defensively.
void append_json_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else {
            out += c;
        }
    }
    out += '"';
}

}  // namespace

std::optional<double> OptionsPayload::get_number(std::string_view name) const {
    for (const auto& [k, v] : fields) {
        if (k == name && v.kind == OptionValue::Kind::Number) {
            return v.num;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> OptionsPayload::get_string(std::string_view name) const {
    for (const auto& [k, v] : fields) {
        if (k == name && v.kind == OptionValue::Kind::String) {
            return std::string_view(v.str);
        }
    }
    return std::nullopt;
}

std::optional<bool> OptionsPayload::get_bool(std::string_view name) const {
    for (const auto& [k, v] : fields) {
        if (k == name && v.kind == OptionValue::Kind::Bool) {
            return v.b;
        }
    }
    return std::nullopt;
}

std::string OptionsPayload::to_json() const {
    if (fields.empty()) return "";

    std::string out;
    out.reserve(64);
    out += '{';
    bool first = true;
    for (const auto& [name, v] : fields) {
        if (!first) out += ',';
        first = false;
        append_json_string(out, name);
        out += ':';
        switch (v.kind) {
            case OptionValue::Kind::Number: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", v.num);
                out += buf;
                break;
            }
            case OptionValue::Kind::String:
                append_json_string(out, v.str);
                break;
            case OptionValue::Kind::Bool:
                out += v.b ? "true" : "false";
                break;
        }
    }
    out += '}';
    return out;
}

OptionsPayload extract_options(const AstArena&     arena,
                               NodeIndex           arg_node,
                               const OptionSchema& schema) {
    OptionsPayload payload;
    if (arg_node == NULL_NODE) return payload;

    NodeIndex value_node = arg_node;
    {
        const Node& wrapper = arena[arg_node];
        if (wrapper.type == NodeType::Argument) {
            value_node = wrapper.first_child;
        }
    }
    if (value_node == NULL_NODE) return payload;

    const Node& rec = arena[value_node];
    if (rec.type != NodeType::RecordLit) return payload;

    for (NodeIndex field_node = rec.first_child;
         field_node != NULL_NODE;
         field_node = arena[field_node].next_sibling) {

        const Node& field = arena[field_node];
        if (field.type != NodeType::Argument) continue;
        if (!std::holds_alternative<Node::RecordFieldData>(field.data)) continue;

        const auto& field_data = field.as_record_field();
        std::string_view name(field_data.name);

        const NodeIndex value_child = field.first_child;
        if (value_child == NULL_NODE) continue;
        const Node& val = arena[value_child];

        OptionValue ov;
        bool captured = false;
        switch (val.type) {
            case NodeType::NumberLit:
                ov.kind = OptionValue::Kind::Number;
                ov.num  = val.as_number();
                captured = true;
                break;
            case NodeType::StringLit:
                ov.kind = OptionValue::Kind::String;
                ov.str  = std::string(val.as_string());
                captured = true;
                break;
            case NodeType::BoolLit:
                ov.kind = OptionValue::Kind::Bool;
                ov.b    = val.as_bool();
                captured = true;
                break;
            default:
                // Non-literal value (e.g. {fft: some_var}). The pre-Phase-5
                // helper silently dropped these — preserve that.
                break;
        }
        if (!captured) continue;

        if (schema.field_count == 0 || schema_has_field(schema, name)) {
            payload.fields.emplace_back(name, std::move(ov));
        } else {
            payload.unknown_fields.push_back(name);
        }
    }

    return payload;
}

}  // namespace codegen
}  // namespace akkado
