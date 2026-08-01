#include "akkado/expr_kinds.hpp"

#include <variant>

namespace akkado::expr_kinds {

bool is_pattern_producer(const AstArena& arena, NodeIndex node,
                         const StringInterner& interner) {
    if (node == NULL_NODE) return false;
    const Node& n = arena[node];
    switch (n.type) {
        case NodeType::MiniLiteral:
            return true;
        case NodeType::MethodCall:
            // Method calls preserve pattern-ness of the receiver
            // (.slow/.fast/.rev/.transpose/etc. all return patterns).
            return is_pattern_producer(arena, n.first_child, interner);
        case NodeType::Call:
            // Call nodes carry the callee name in IdentifierData.
            if (std::holds_alternative<Node::IdentifierData>(n.data)) {
                return is_pattern_producing_call_name(
                    interner.view(n.as_identifier()));
            }
            return false;
        default:
            return false;
    }
}

std::optional<ConstValue> try_const_value(const AstArena& arena,
                                          NodeIndex node,
                                          const SymbolTable& symbols) {
    if (node == NULL_NODE) return std::nullopt;

    const Node& n = arena[node];
    switch (n.type) {
        case NodeType::NumberLit:
            return ConstValue{n.as_number()};

        case NodeType::BoolLit:
            return ConstValue{n.as_bool() ? 1.0 : 0.0};

        case NodeType::PitchLit:
            return ConstValue{midi_to_hz(static_cast<double>(n.as_pitch()))};

        case NodeType::Identifier: {
            // Lookup by SymbolId — no rehash (correctness PRD Phase 5 / F12).
            if (std::holds_alternative<Node::IdentifierData>(n.data)) {
                auto sym = symbols.lookup(n.as_identifier());
                if (sym && sym->is_const && sym->const_value.has_value()) {
                    return *sym->const_value;
                }
            }
            return std::nullopt;
        }

        case NodeType::ArrayLit: {
            std::vector<double> elements;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                auto val = try_const_value(arena, child, symbols);
                if (!val) return std::nullopt;
                if (!std::holds_alternative<double>(*val)) return std::nullopt;
                elements.push_back(std::get<double>(*val));
                child = arena[child].next_sibling;
            }
            return ConstValue{std::move(elements)};
        }

        default:
            return std::nullopt;
    }
}

}  // namespace akkado::expr_kinds
