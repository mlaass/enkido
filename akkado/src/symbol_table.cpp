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
    auto [_, inserted] = current.try_emplace(symbol.name_hash, symbol);
    return inserted;
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
