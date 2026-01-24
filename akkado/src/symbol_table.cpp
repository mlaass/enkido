#include "akkado/symbol_table.hpp"

namespace akkado {

SymbolTable::SymbolTable() {
    // Start with global scope containing builtins
    scopes_.emplace_back();
    register_builtins();
}

void SymbolTable::push_scope() {
    scopes_.emplace_back();
}

void SymbolTable::pop_scope() {
    if (scopes_.size() > 1) {
        scopes_.pop_back();
    }
}

bool SymbolTable::define(const Symbol& symbol) {
    auto& current = scopes_.back();
    bool was_new = current.find(symbol.name_hash) == current.end();
    current.insert_or_assign(symbol.name_hash, symbol);
    return was_new;
}

bool SymbolTable::define_variable(std::string_view name, std::uint16_t buffer_index) {
    Symbol sym{};
    sym.kind = SymbolKind::Variable;
    sym.name_hash = fnv1a_hash(name);
    sym.name = std::string(name);
    sym.buffer_index = buffer_index;
    return define(sym);
}

bool SymbolTable::define_parameter(std::string_view name, std::uint16_t buffer_index) {
    Symbol sym{};
    sym.kind = SymbolKind::Parameter;
    sym.name_hash = fnv1a_hash(name);
    sym.name = std::string(name);
    sym.buffer_index = buffer_index;
    return define(sym);
}

bool SymbolTable::define_function(const UserFunctionInfo& func_info) {
    Symbol sym{};
    sym.kind = SymbolKind::UserFunction;
    sym.name_hash = fnv1a_hash(func_info.name);
    sym.name = func_info.name;
    sym.buffer_index = 0xFFFF;  // Not applicable for functions
    sym.user_function = func_info;
    return define(sym);
}

std::optional<Symbol> SymbolTable::lookup(std::string_view name) const {
    return lookup(fnv1a_hash(name));
}

std::optional<Symbol> SymbolTable::lookup(std::uint32_t name_hash) const {
    // Search from innermost scope outward
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name_hash);
        if (found != it->end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

bool SymbolTable::is_defined_in_current_scope(std::string_view name) const {
    if (scopes_.empty()) return false;
    auto hash = fnv1a_hash(name);
    return scopes_.back().find(hash) != scopes_.back().end();
}

void SymbolTable::update_function_nodes(const std::unordered_map<NodeIndex, NodeIndex>& node_map) {
    // Iterate through all scopes and update UserFunction entries
    for (auto& scope : scopes_) {
        for (auto& [hash, sym] : scope) {
            if (sym.kind == SymbolKind::UserFunction) {
                // Update body_node
                auto body_it = node_map.find(sym.user_function.body_node);
                if (body_it != node_map.end()) {
                    sym.user_function.body_node = body_it->second;
                }
                // Update def_node
                auto def_it = node_map.find(sym.user_function.def_node);
                if (def_it != node_map.end()) {
                    sym.user_function.def_node = def_it->second;
                }
            }
        }
    }
}

void SymbolTable::register_builtins() {
    // Register all built-in functions from the builtins table
    for (const auto& [name, info] : BUILTIN_FUNCTIONS) {
        Symbol sym{};
        sym.kind = SymbolKind::Builtin;
        sym.name_hash = fnv1a_hash(name);
        sym.name = std::string(name);
        sym.buffer_index = 0xFFFF;  // Not applicable for builtins
        sym.builtin = info;
        define(sym);
    }

    // Also register aliases
    for (const auto& [alias, canonical] : BUILTIN_ALIASES) {
        auto sym_opt = lookup(canonical);
        if (sym_opt) {
            Symbol alias_sym = *sym_opt;
            alias_sym.name_hash = fnv1a_hash(alias);
            alias_sym.name = std::string(alias);
            define(alias_sym);
        }
    }
}

} // namespace akkado
