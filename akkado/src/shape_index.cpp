#include "akkado/shape_index.hpp"

#include "akkado/ast.hpp"
#include "akkado/expr_kinds.hpp"
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"
#include "akkado/typed_value.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

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

// FNV-1a 32-bit — same scheme as the editor side and as
// symbol_table.hpp::fnv1a_hash. Kept private to this TU.
std::uint32_t hash_source(std::string_view src) {
    std::uint32_t h = 2166136261u;
    for (char c : src) {
        h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    return h;
}

// Map a field's underlying RHS node type to the textual label exposed in
// the shape JSON. Conservative — anything we don't statically recognise
// becomes "unknown", and the editor falls back to a generic completion.
const char* type_label_for_rhs_node(const Ast& ast, NodeIndex rhs) {
    if (rhs == NULL_NODE) return "unknown";
    const Node& n = ast.arena[rhs];
    switch (n.type) {
        case NodeType::NumberLit:   return "number";
        case NodeType::BoolLit:     return "bool";
        case NodeType::StringLit:   return "string";
        case NodeType::PitchLit:    return "number";
        case NodeType::RecordLit:   return "record";
        case NodeType::ArrayLit:    return "array";
        case NodeType::MiniLiteral: return "pattern";
        default:                    return "unknown";
    }
}

void emit_record_shape_from_ast(std::ostringstream& json,
                                const Ast& ast,
                                NodeIndex record_node) {
    json << "\"fields\":[";
    bool first = true;

    // Spread source: collect spread fields first; explicit fields can
    // shadow them but order doesn't strictly matter for autocomplete.
    if (record_node != NULL_NODE) {
        const Node& rec = ast.arena[record_node];
        {
            NodeIndex spread_src = rec.extra_child(0);
            if (spread_src != NULL_NODE) {
                const Node& spread = ast.arena[spread_src];
                if (spread.type == NodeType::RecordLit) {
                    NodeIndex sf = spread.first_child;
                    while (sf != NULL_NODE) {
                        const Node& sfield = ast.arena[sf];
                        if (sfield.type == NodeType::Argument &&
                            std::holds_alternative<Node::RecordFieldData>(sfield.data)) {
                            if (!first) json << ",";
                            first = false;
                            const auto& fd = sfield.as_record_field();
                            const char* label = "unknown";
                            if (sfield.first_child != NULL_NODE) {
                                label = type_label_for_rhs_node(ast, sfield.first_child);
                            }
                            json << "{\"name\":\"" << escape_json(fd.name) << "\""
                                 << ",\"type\":\"" << label << "\"}";
                        }
                        sf = ast.arena[sf].next_sibling;
                    }
                }
                // For Identifier-based spread (`{..base, ...}`) we'd need to
                // resolve `base` recursively; deferred to v2 since it
                // requires symbol-table cooperation.
            }
        }

        // Explicit fields.
        NodeIndex field_node = rec.first_child;
        std::unordered_set<std::string> seen;
        while (field_node != NULL_NODE) {
            const Node& field = ast.arena[field_node];
            if (field.type == NodeType::Argument &&
                std::holds_alternative<Node::RecordFieldData>(field.data)) {
                const auto& fd = field.as_record_field();
                if (seen.insert(fd.name).second) {
                    if (!first) json << ",";
                    first = false;
                    const char* label = "unknown";
                    if (field.first_child != NULL_NODE) {
                        label = type_label_for_rhs_node(ast, field.first_child);
                    }
                    json << "{\"name\":\"" << escape_json(fd.name) << "\""
                         << ",\"type\":\"" << label << "\"}";
                }
            }
            field_node = ast.arena[field_node].next_sibling;
        }
    }
    json << "]";
}

// True when `rhs` is a method-call chain rooted at a pattern producer or
// a direct pattern producer. Delegates to the canonical
// expr_kinds::is_pattern_producer (Phase 7 — one list, no local copy).
bool rhs_is_pattern(const Ast& ast, NodeIndex rhs,
                    const StringInterner& interner) {
    return expr_kinds::is_pattern_producer(ast.arena, rhs, interner);
}

// Walk a method-call chain from `outer` towards the receiver, collecting
// the first-string-arg of every `.set("name", …)` call. Names appear in
// chain order (outermost first); duplicates kept by insertion order.
std::vector<std::string> collect_chain_set_names(const Ast& ast, NodeIndex outer,
                                                  const StringInterner& interner) {
    std::vector<std::string> names;
    std::vector<NodeIndex> stack;
    NodeIndex cur = outer;
    while (cur != NULL_NODE && ast.arena[cur].type == NodeType::MethodCall) {
        stack.push_back(cur);
        cur = ast.arena[cur].first_child;
    }
    // Walk inner-to-outer so the order matches source order.
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        const Node& mc = ast.arena[*it];
        if (!std::holds_alternative<Node::IdentifierData>(mc.data)) continue;
        if (interner.view(mc.as_identifier()) != "set") continue;
        // First arg sits as the second child (after the receiver).
        NodeIndex arg = ast.arena[*it].first_child;
        if (arg != NULL_NODE) arg = ast.arena[arg].next_sibling;
        if (arg == NULL_NODE) continue;
        const Node& arg_n = ast.arena[arg];
        // Argument wraps an expression; the value sits as its first child.
        NodeIndex value = arg_n.first_child;
        if (arg_n.type == NodeType::StringLit) value = arg;
        if (value == NULL_NODE) continue;
        const Node& v = ast.arena[value];
        if (v.type == NodeType::StringLit) {
            names.push_back(v.as_string());
        }
    }
    return names;
}

void emit_pattern_fields(std::ostringstream& json,
                         const std::vector<std::string>& custom_field_names) {
    json << "\"fields\":[";
    bool first = true;
    std::unordered_set<std::string> reserved;

    for (const auto& row : pattern_field_aliases()) {
        if (row.names.empty()) continue;
        const std::string canonical = row.names.front();
        bool is_canonical = true;
        for (const char* alias : row.names) {
            reserved.insert(alias);
            if (!first) json << ",";
            first = false;
            json << "{\"name\":\"" << escape_json(alias) << "\""
                 << ",\"type\":\"signal\""
                 << ",\"fixed\":true";
            if (!is_canonical) {
                json << ",\"aliasOf\":\"" << escape_json(canonical) << "\"";
            }
            json << "}";
            is_canonical = false;
        }
    }

    std::unordered_set<std::string> emitted_custom;
    for (const auto& name : custom_field_names) {
        if (reserved.find(name) != reserved.end()) continue;  // collision dedupe
        if (!emitted_custom.insert(name).second) continue;     // dedupe within chain
        if (!first) json << ",";
        first = false;
        json << "{\"name\":\"" << escape_json(name) << "\""
             << ",\"type\":\"signal\""
             << ",\"fixed\":false"
             << ",\"source\":\"set\"}";
    }
    json << "]";
}

// True iff every child of `arr_node` is a RecordLit (no spread, no nested
// arrays). Returns the first element's RecordLit index when true.
std::pair<bool, NodeIndex> array_all_records(const Ast& ast, NodeIndex arr_node) {
    if (arr_node == NULL_NODE) return {false, NULL_NODE};
    const Node& n = ast.arena[arr_node];
    if (n.type != NodeType::ArrayLit) return {false, NULL_NODE};
    NodeIndex first_record = NULL_NODE;
    NodeIndex elem = n.first_child;
    int count = 0;
    while (elem != NULL_NODE) {
        const Node& en = ast.arena[elem];
        if (en.type != NodeType::RecordLit) return {false, NULL_NODE};
        if (first_record == NULL_NODE) first_record = elem;
        ++count;
        elem = ast.arena[elem].next_sibling;
    }
    return {count > 0, first_record};
}

void emit_binding(std::ostringstream& json,
                  const Ast& ast,
                  const StringInterner& interner,
                  std::string_view name,
                  NodeIndex rhs) {
    json << "\"" << escape_json(name) << "\":{";
    if (rhs == NULL_NODE) {
        json << "\"kind\":\"unknown\",\"fields\":[]}";
        return;
    }
    const Node& n = ast.arena[rhs];

    if (n.type == NodeType::RecordLit) {
        json << "\"kind\":\"record\",";
        emit_record_shape_from_ast(json, ast, rhs);
        json << "}";
        return;
    }

    if (rhs_is_pattern(ast, rhs, interner)) {
        json << "\"kind\":\"pattern\",";
        std::vector<std::string> custom_names;
        if (n.type == NodeType::MethodCall) {
            custom_names = collect_chain_set_names(ast, rhs, interner);
        }
        emit_pattern_fields(json, custom_names);
        json << "}";
        return;
    }

    if (n.type == NodeType::ArrayLit) {
        auto [all_records, first_rec] = array_all_records(ast, rhs);
        if (all_records) {
            json << "\"kind\":\"array\",\"elementKind\":\"record\",";
            emit_record_shape_from_ast(json, ast, first_rec);
        } else {
            json << "\"kind\":\"array\",\"fields\":[]";
        }
        json << "}";
        return;
    }

    json << "\"kind\":\"unknown\",\"fields\":[]}";
}

// Determine whether a Program-child node represents a top-level assignment
// the editor should expose as a binding. Returns the assignment's name and
// RHS node when so; {nullopt, NULL_NODE} otherwise.
struct AssignmentRow {
    std::string name;
    NodeIndex rhs;
};

bool extract_top_level_assignment(const Ast& ast, NodeIndex idx,
                                  const StringInterner& interner,
                                  AssignmentRow& out) {
    if (idx == NULL_NODE) return false;
    const Node& n = ast.arena[idx];
    if (n.type != NodeType::Assignment) return false;
    if (!std::holds_alternative<Node::IdentifierData>(n.data)) return false;
    out.name = std::string(interner.view(n.as_identifier()));
    out.rhs = n.first_child;
    return true;
}

// Walk siblings of a Program node to collect top-level assignments.
std::vector<AssignmentRow> collect_top_level_assignments(const Ast& ast,
                                                          const StringInterner& interner) {
    std::vector<AssignmentRow> rows;
    if (ast.root == NULL_NODE) return rows;
    const Node& prog = ast.arena[ast.root];
    NodeIndex stmt = prog.first_child;
    std::unordered_set<std::string> seen;
    while (stmt != NULL_NODE) {
        AssignmentRow row;
        if (extract_top_level_assignment(ast, stmt, interner, row)) {
            if (seen.insert(row.name).second) {
                rows.push_back(std::move(row));
            }
        }
        stmt = ast.arena[stmt].next_sibling;
    }
    return rows;
}

// True when `cursor` falls within the source range of `idx` or any of its
// descendants. The Pipe node only stores the `|>` operator's location, so
// we cannot rely on Node::location for Pipe — descend instead.
bool subtree_covers(const Ast& ast, NodeIndex idx, std::uint32_t cursor) {
    if (idx == NULL_NODE) return false;
    const Node& n = ast.arena[idx];
    const auto start = n.location.offset;
    const auto end   = n.location.offset + n.location.length;
    if (cursor >= start && cursor < end) return true;
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        if (subtree_covers(ast, child, cursor)) return true;
        child = ast.arena[child].next_sibling;
    }
    return false;
}

// Walk the AST and find the deepest Pipe whose subtree covers `cursor`.
NodeIndex deepest_pipe_covering(const Ast& ast,
                                NodeIndex idx,
                                std::uint32_t cursor) {
    if (idx == NULL_NODE) return NULL_NODE;
    const Node& n = ast.arena[idx];

    NodeIndex best = NULL_NODE;
    if (n.type == NodeType::Pipe && subtree_covers(ast, idx, cursor)) {
        best = idx;
    }

    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        NodeIndex deeper = deepest_pipe_covering(ast, child, cursor);
        if (deeper != NULL_NODE) best = deeper;
        child = ast.arena[child].next_sibling;
    }
    return best;
}

// Resolve the LHS of a Pipe to a binding RHS in the AST. Returns NULL_NODE
// when the LHS is anything other than a simple identifier reference (or a
// PipeBinding wrapping one) that maps to a known top-level assignment.
NodeIndex resolve_pipe_lhs_rhs(const Ast& ast,
                               NodeIndex pipe_idx,
                               const StringInterner& interner,
                               const std::vector<AssignmentRow>& bindings) {
    if (pipe_idx == NULL_NODE) return NULL_NODE;
    const Node& pipe = ast.arena[pipe_idx];
    if (pipe.type != NodeType::Pipe) return NULL_NODE;

    NodeIndex lhs_idx = pipe.first_child;
    if (lhs_idx == NULL_NODE) return NULL_NODE;
    const Node& lhs = ast.arena[lhs_idx];

    auto find_binding = [&](const std::string& name) -> NodeIndex {
        for (const auto& row : bindings) {
            if (row.name == name) return row.rhs;
        }
        return NULL_NODE;
    };

    if (lhs.type == NodeType::Identifier) {
        return find_binding(std::string(interner.view(lhs.as_identifier())));
    }

    // `expr as e |> ...`: the LHS is a PipeBinding wrapping the producer.
    if (lhs.type == NodeType::PipeBinding) {
        NodeIndex inner = lhs.first_child;
        if (inner == NULL_NODE) return NULL_NODE;
        const Node& inner_n = ast.arena[inner];
        if (inner_n.type == NodeType::Identifier) {
            return find_binding(std::string(interner.view(inner_n.as_identifier())));
        }
        // The producer is an inline pat(...) or method-chain — return the
        // node directly so emit_binding-style logic can read it.
        return inner;
    }

    // Inline pattern producer in pipe position: `pat("c4 e4") |> osc(...)`.
    return lhs_idx;
}

}  // namespace

std::string shape_index_json(std::string_view source,
                             std::uint32_t cursor_offset) {
    std::ostringstream json;
    json << "{\"version\":1,\"sourceHash\":" << hash_source(source);

    // PRD prd-parser-codegen-correctness.md Phase 5 (F12): shape_index
    // re-lexes/re-parses for its own purposes (audit F1 / PRD-2 covers
    // sharing the main compile's AST). It owns a local interner so
    // identifier tokens get SymbolIds — required by the post-Phase-5
    // Lexer/Parser ctor signatures.
    StringInterner interner;

    auto [tokens, lex_diags] = lex(source, interner, "<shape-index>");
    if (tokens.empty()) {
        json << ",\"bindings\":{}}";
        return json.str();
    }

    auto [ast, parse_diags] = parse(std::move(tokens), source, interner, "<shape-index>");

    auto bindings = collect_top_level_assignments(ast, interner);

    json << ",\"bindings\":{";
    bool first = true;
    for (const auto& row : bindings) {
        if (row.rhs == NULL_NODE) continue;
        const Node& rhs_n = ast.arena[row.rhs];
        // Only Record / Pattern / Array bindings expose a shape. Skip
        // everything else so the JSON stays small (numbers, strings, etc.
        // need no completion data).
        const bool is_record = (rhs_n.type == NodeType::RecordLit);
        const bool is_pattern = rhs_is_pattern(ast, row.rhs, interner);
        const bool is_array = (rhs_n.type == NodeType::ArrayLit);
        if (!is_record && !is_pattern && !is_array) continue;

        if (!first) json << ",";
        first = false;
        emit_binding(json, ast, interner, row.name, row.rhs);
    }
    json << "}";

    if (cursor_offset != SHAPE_INDEX_NO_CURSOR) {
        NodeIndex pipe_idx = deepest_pipe_covering(ast, ast.root, cursor_offset);
        if (pipe_idx != NULL_NODE) {
            NodeIndex producer = resolve_pipe_lhs_rhs(ast, pipe_idx, interner, bindings);
            if (producer != NULL_NODE && rhs_is_pattern(ast, producer, interner)) {
                std::vector<std::string> custom_names;
                if (ast.arena[producer].type == NodeType::MethodCall) {
                    custom_names = collect_chain_set_names(ast, producer, interner);
                }
                json << ",\"patternHole\":{\"kind\":\"pattern\",";
                emit_pattern_fields(json, custom_names);
                json << "}";
            }
        }
    }

    json << "}";
    return json.str();
}

}  // namespace akkado
