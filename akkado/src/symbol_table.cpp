#include "akkado/symbol_table.hpp"

namespace akkado {

SymbolTable::SymbolTable() {
    // PRD prd-parser-codegen-correctness.md Phase 5 (F12): the default
    // ctor leaves the table empty (one initial scope, no builtins).
    // Production code calls register_builtins() after attaching an
    // interner. Tests that exercise scope mechanics use this form.
    scopes_.emplace_back();
}

SymbolTable::SymbolTable(StringInterner& interner) {
    scopes_.emplace_back();
    register_builtins(interner);
}

void SymbolTable::register_builtins(StringInterner& interner) {
    interner_ = &interner;

    // Register all built-in functions from the builtins table
    for (const auto& [name, info] : BUILTIN_FUNCTIONS) {
        // notes/freqs (pattern-event-arrays PRD) are codegen-dispatched but
        // intentionally NOT pre-registered as global symbols: they are common
        // variable names and must remain bindable (`notes = [...]`). The
        // analyzer special-cases the call form; codegen's special_handlers
        // map dispatches it.
        if (name == "notes" || name == "freqs") continue;
        Symbol sym{};
        sym.kind = SymbolKind::Builtin;
        sym.name_id = interner.intern(name);
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
            alias_sym.name_id = interner.intern(alias);
            alias_sym.name = std::string(alias);
            define(alias_sym);
        }
    }
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
    bool was_new = current.find(symbol.name_id) == current.end();
    current.insert_or_assign(symbol.name_id, symbol);
    return was_new;
}

bool SymbolTable::define_variable(std::string_view name, std::uint16_t buffer_index) {
    Symbol sym{};
    sym.kind = SymbolKind::Variable;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = buffer_index;
    return define(sym);
}

bool SymbolTable::define_parameter(std::string_view name, std::uint16_t buffer_index) {
    Symbol sym{};
    sym.kind = SymbolKind::Parameter;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = buffer_index;
    return define(sym);
}

bool SymbolTable::define_function(const UserFunctionInfo& func_info) {
    Symbol sym{};
    sym.kind = SymbolKind::UserFunction;
    sym.name_id = interner_->intern(func_info.name);
    sym.name = func_info.name;
    sym.buffer_index = 0xFFFF;  // Not applicable for functions
    sym.user_function = func_info;
    return define(sym);
}

bool SymbolTable::define_pattern(std::string_view name, const PatternInfo& pattern_info) {
    Symbol sym{};
    sym.kind = SymbolKind::Pattern;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = 0xFFFF;  // Patterns don't have a single buffer
    sym.pattern = pattern_info;
    return define(sym);
}

bool SymbolTable::define_array(std::string_view name, const ArrayInfo& array_info) {
    Symbol sym{};
    sym.kind = SymbolKind::Array;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = 0xFFFF;  // Arrays use array.buffer_indices instead
    sym.array = array_info;
    return define(sym);
}

bool SymbolTable::define_function_value(std::string_view name, const FunctionRef& func_ref) {
    Symbol sym{};
    sym.kind = SymbolKind::FunctionValue;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = 0xFFFF;  // Function values don't have a buffer
    sym.function_ref = func_ref;
    return define(sym);
}

bool SymbolTable::define_record(std::string_view name, std::shared_ptr<RecordTypeInfo> record_type) {
    Symbol sym{};
    sym.kind = SymbolKind::Record;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = 0xFFFF;  // Records don't have a single buffer
    sym.record_type = std::move(record_type);
    return define(sym);
}

bool SymbolTable::define_const_variable(std::string_view name, const ConstValue& value) {
    Symbol sym{};
    sym.kind = SymbolKind::Variable;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = 0xFFFF;  // Will be assigned during codegen
    sym.is_const = true;
    sym.const_value = value;
    return define(sym);
}

bool SymbolTable::define_const_placeholder(std::string_view name) {
    Symbol sym{};
    sym.kind = SymbolKind::Variable;
    sym.name_id = interner_->intern(name);
    sym.name = std::string(name);
    sym.buffer_index = 0xFFFF;
    sym.is_const = true;
    // const_value remains std::nullopt — not yet initialized
    return define(sym);
}

std::optional<Symbol> SymbolTable::lookup(std::string_view name) const {
    // PRD Phase 5: route through find() — an absent name does not
    // bloat the interner.
    SymbolId id = interner_ ? interner_->find(name) : NULL_SYMBOL;
    if (id == NULL_SYMBOL) return std::nullopt;
    return lookup(id);
}

std::optional<Symbol> SymbolTable::lookup(SymbolId id) const {
    if (id == NULL_SYMBOL) return std::nullopt;
    // Search from innermost scope outward
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(id);
        if (found != it->end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

bool SymbolTable::is_defined_in_current_scope(std::string_view name) const {
    if (scopes_.empty() || !interner_) return false;
    SymbolId id = interner_->find(name);
    if (id == NULL_SYMBOL) return false;
    return scopes_.back().find(id) != scopes_.back().end();
}

static void update_symbol_nodes(Symbol& sym, const std::unordered_map<NodeIndex, NodeIndex>& node_map) {
    if (sym.kind == SymbolKind::UserFunction) {
        auto body_it = node_map.find(sym.user_function.body_node);
        if (body_it != node_map.end()) sym.user_function.body_node = body_it->second;
        auto def_it = node_map.find(sym.user_function.def_node);
        if (def_it != node_map.end()) sym.user_function.def_node = def_it->second;
        for (auto& param : sym.user_function.params) {
            if (param.default_node != NULL_NODE) {
                auto param_it = node_map.find(param.default_node);
                if (param_it != node_map.end()) param.default_node = param_it->second;
            }
            // Phase 3b: each destructure field's default expression also
            // moved during the analyzer's AST clone.
            for (auto& f : param.destructure_fields) {
                if (f.default_node != NULL_NODE) {
                    auto it = node_map.find(f.default_node);
                    if (it != node_map.end()) f.default_node = it->second;
                }
            }
        }
    } else if (sym.kind == SymbolKind::FunctionValue) {
        auto closure_it = node_map.find(sym.function_ref.closure_node);
        if (closure_it != node_map.end()) sym.function_ref.closure_node = closure_it->second;
        for (auto& param : sym.function_ref.params) {
            if (param.default_node != NULL_NODE) {
                auto param_it = node_map.find(param.default_node);
                if (param_it != node_map.end()) param.default_node = param_it->second;
            }
            // A `name = ({x = expr}) -> …` closure carries per-field default
            // expressions that moved during the analyzer's AST clone, same as
            // the UserFunction branch above.
            for (auto& f : param.destructure_fields) {
                if (f.default_node != NULL_NODE) {
                    auto it = node_map.find(f.default_node);
                    if (it != node_map.end()) f.default_node = it->second;
                }
            }
        }
    } else if (sym.kind == SymbolKind::Pattern) {
        auto pat_it = node_map.find(sym.pattern.pattern_node);
        if (pat_it != node_map.end()) sym.pattern.pattern_node = pat_it->second;
    } else if (sym.kind == SymbolKind::Array) {
        auto arr_it = node_map.find(sym.array.source_node);
        if (arr_it != node_map.end()) sym.array.source_node = arr_it->second;
    } else if (sym.kind == SymbolKind::Record && sym.record_type) {
        auto rec_it = node_map.find(sym.record_type->source_node);
        if (rec_it != node_map.end()) sym.record_type->source_node = rec_it->second;
    }
}

void SymbolTable::update_function_nodes(const std::unordered_map<NodeIndex, NodeIndex>& node_map) {
    // Iterate through all scopes and update UserFunction, FunctionValue, and Pattern entries
    for (auto& scope : scopes_) {
        for (auto& [id, sym] : scope) {
            update_symbol_nodes(sym, node_map);
        }
    }
    // Also update hidden symbols (namespace imports)
    for (auto& [mod_path, mod_symbols] : hidden_symbols_) {
        for (auto& [id, sym] : mod_symbols) {
            update_symbol_nodes(sym, node_map);
        }
    }
}

bool SymbolTable::define_module(std::string_view alias, std::string_view canonical_path) {
    Symbol sym{};
    sym.kind = SymbolKind::Module;
    sym.name_id = interner_->intern(alias);
    sym.name = std::string(alias);
    sym.buffer_index = 0xFFFF;
    sym.module_path = std::string(canonical_path);
    return define(sym);
}

std::optional<Symbol> SymbolTable::lookup_in_module(
    std::string_view module_path, std::string_view name) const {
    auto mod_it = hidden_symbols_.find(std::string(module_path));
    if (mod_it == hidden_symbols_.end()) return std::nullopt;
    SymbolId id = interner_ ? interner_->find(name) : NULL_SYMBOL;
    if (id == NULL_SYMBOL) return std::nullopt;
    auto sym_it = mod_it->second.find(id);
    if (sym_it == mod_it->second.end()) return std::nullopt;
    return sym_it->second;
}

void SymbolTable::hide_symbol(std::string_view name, std::string_view module_path) {
    if (!interner_) return;
    SymbolId id = interner_->find(name);
    if (id == NULL_SYMBOL) return;
    // Search scopes for the symbol and move it to hidden storage
    for (auto& scope : scopes_) {
        auto it = scope.find(id);
        if (it != scope.end()) {
            hidden_symbols_[std::string(module_path)][id] = it->second;
            scope.erase(it);
            return;
        }
    }
}

} // namespace akkado
