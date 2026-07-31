#include "akkado/builtins_json.hpp"
#include "akkado/builtins.hpp"

#include <cmath>
#include <sstream>
#include <string>
#include <string_view>

namespace akkado {

namespace {

std::string escape_json(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (char c : sv) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

// Find the OptionSchema in `info` that targets parameter slot `param_index`,
// or nullptr when none.
const OptionSchema* find_schema_for_param(const BuiltinInfo& info, std::size_t param_index) {
    for (std::uint8_t s = 0; s < info.option_schema_count; ++s) {
        const OptionSchema& schema = info.option_schemas[s];
        if (schema.param_index == param_index) {
            return &schema;
        }
    }
    return nullptr;
}

void emit_option_fields(std::ostringstream& json, const OptionSchema& schema) {
    json << ",\"optionFields\":[";
    for (std::uint8_t f = 0; f < schema.field_count; ++f) {
        if (f > 0) json << ",";
        const OptionField& fld = schema.fields[f];
        json << "{\"name\":\"" << escape_json(fld.name) << "\""
             << ",\"type\":\"" << option_field_type_name(fld.type) << "\"";
        if (!fld.default_repr.empty()) {
            json << ",\"default\":\"" << escape_json(fld.default_repr) << "\"";
        }
        if (!fld.description.empty()) {
            json << ",\"description\":\"" << escape_json(fld.description) << "\"";
        }
        if (fld.type == OptionFieldType::Enum && !fld.enum_values.empty()) {
            json << ",\"values\":\"" << escape_json(fld.enum_values) << "\"";
        }
        json << "}";
    }
    json << "]";
    json << ",\"acceptsSpread\":" << (schema.accepts_spread ? "true" : "false");
}

}  // namespace

std::string serialize_builtins_json() {
    std::ostringstream json;
    json << "{\"functions\":{";

    bool first_func = true;
    for (const auto& [name, info] : BUILTIN_FUNCTIONS) {
        if (!first_func) json << ",";
        first_func = false;

        json << "\"" << escape_json({name.data(), name.size()}) << "\":{";
        json << "\"params\":[";

        bool first_param = true;
        for (std::size_t i = 0; i < MAX_BUILTIN_PARAMS; ++i) {
            if (info.param_names[i].empty()) break;

            if (!first_param) json << ",";
            first_param = false;

            json << "{\"name\":\"" << escape_json(info.param_names[i]) << "\"";

            const bool is_required = i < info.input_count;
            json << ",\"required\":" << (is_required ? "true" : "false");

            if (!is_required && info.has_default(i)) {
                float def = info.get_default(i);
                json << ",\"default\":" << def;
            }

            if (info.param_types[i] == ParamValueType::Record) {
                json << ",\"type\":\"record\"";
                if (const OptionSchema* schema = find_schema_for_param(info, i)) {
                    emit_option_fields(json, *schema);
                }
            }

            json << "}";
        }

        // Extended params (PRD prd-extended-params §5) — appended after positional
        // ones so the editor's signature-help and autocomplete surface them as
        // additional optional kwargs. Common case: unified dry/wet for effects
        // whose 5 input slots are full.
        const std::size_t base = info.total_params();
        for (std::uint8_t e = 0;
             e < info.extended_param_count && e < MAX_EXTENDED_PARAMS; ++e) {
            if (info.extended_param_names[e].empty()) break;

            if (!first_param) json << ",";
            first_param = false;

            const std::size_t idx = base + e;
            json << "{\"name\":\"" << escape_json(info.extended_param_names[e]) << "\""
                 << ",\"required\":false"
                 << ",\"extended\":true";
            if (info.has_default(idx)) {
                json << ",\"default\":" << info.get_default(idx);
            }
            json << "}";
        }

        json << "],\"description\":\"" << escape_json(info.description) << "\"}";
    }

    json << "},\"aliases\":{";

    bool first_alias = true;
    for (const auto& [alias, canonical] : BUILTIN_ALIASES) {
        if (!first_alias) json << ",";
        first_alias = false;
        json << "\"" << escape_json({alias.data(), alias.size()}) << "\":\"" << escape_json(canonical) << "\"";
    }

    json << "},\"keywords\":[\"fn\",\"seq\",\"timeline\",\"true\",\"false\",\"match\",\"post\"]}";

    return json.str();
}

}  // namespace akkado
