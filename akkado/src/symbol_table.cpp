#include "akkado/symbol_table.hpp"
#ifdef CEDAR_HOST_EXTENSIONS
#include "akkado/host_extensions.hpp"
#endif

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
    // Phase 3: no per-construction builtin registration — lookups fall
    // back to the process-shared builtin_scope().
    interner_ = &interner;
    use_builtins_ = true;
}

const std::unordered_map<std::string_view, Symbol>& builtin_scope() {
    // Hardening PRD Phase 3 (PRD-12): process-shared builtin scope, built
    // exactly once (C++11 static-init guarantees one-shot construction even
    // under concurrent first use). Keys are views into the frozen tables'
    // static storage; `name_id` stays NULL_SYMBOL because SymbolIds are
    // per-compile — SymbolTable::lookup_builtin patches the caller's id onto
    // the returned copy. Host-extension builtins are NOT baked in here: the
    // host registry can gain entries between compiles, so lookup_builtin
    // consults it live.
    static const std::unordered_map<std::string_view, Symbol> scope = [] {
        std::unordered_map<std::string_view, Symbol> s;
        s.reserve(BUILTIN_FUNCTIONS.size() + BUILTIN_ALIASES.size());
        for (const auto& [name, info] : BUILTIN_FUNCTIONS) {
            std::string_view sv{name.data(), name.size()};
            // notes/freqs (pattern-event-arrays PRD) are codegen-dispatched
            // but intentionally NOT pre-registered as global symbols: they
            // are common variable names and must remain bindable
            // (`notes = [...]`). The analyzer special-cases the call form;
            // codegen's special_handlers map dispatches it.
            if (sv == "notes" || sv == "freqs") continue;
            Symbol sym{};
            sym.kind = SymbolKind::Builtin;
            sym.name = std::string(sv);
            sym.buffer_index = 0xFFFF;  // Not applicable for builtins
            sym.builtin = info;
            s.emplace(sv, std::move(sym));
        }
        // Aliases resolve after functions (preserves the historical
        // BUILTIN_FUNCTIONS-then-BUILTIN_ALIASES registration order).
        for (const auto& [alias, canonical] : BUILTIN_ALIASES) {
            auto it = s.find(canonical);
            if (it == s.end()) continue;
            std::string_view sv{alias.data(), alias.size()};
            Symbol alias_sym = it->second;
            alias_sym.name = std::string(sv);
            s.emplace(sv, std::move(alias_sym));
        }
        return s;
    }();
    return scope;
}

void SymbolTable::register_builtins(StringInterner& interner) {
    // Hardening PRD Phase 3: builtins live in the process-shared
    // builtin_scope() and are consulted as a lookup fallback — nothing is
    // inserted per SymbolTable anymore. This just arms the fallback.
    interner_ = &interner;
    use_builtins_ = true;
}

std::optional<Symbol> SymbolTable::lookup_builtin(std::string_view name, SymbolId id) const {
    const auto& bs = builtin_scope();
    auto it = bs.find(name);
    if (it != bs.end()) {
        Symbol sym = it->second;
        sym.name_id = id;  // patch the per-compile id onto the shared copy
        return sym;
    }
#ifdef CEDAR_HOST_EXTENSIONS
    // Embedder-registered names resolve exactly like core builtins. Collisions
    // were rejected at registration, so nothing above can be shadowed here.
    if (const BuiltinInfo* info = lookup_host_builtin(name)) {
        Symbol sym{};
        sym.kind = SymbolKind::Builtin;
        sym.name_id = id;
        sym.name = std::string(name);
        sym.buffer_index = 0xFFFF;
        sym.builtin = *info;
        return sym;
    }
#endif
    return std::nullopt;
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

// Phase 4: two user-fn definitions share a signature when their parameter
// *shapes* match — same arity, same ordered annotated-type list, same
// per-param required-ness and rest/destructure flags. Parameter *names* and
// default *values* do not affect identity, but required-ness does (so `f(a)`
// and `f(a = 0)` are distinct overloads). Equal signatures replace; differing
// signatures accumulate.
static bool same_signature(const UserFunctionInfo& a, const UserFunctionInfo& b) {
    if (a.params.size() != b.params.size()) return false;
    if (a.has_rest_param != b.has_rest_param) return false;
    auto required = [](const FunctionParamInfo& p) {
        return !p.default_value.has_value() && !p.default_string.has_value() &&
               p.default_node == NULL_NODE && !p.is_rest;
    };
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        const auto& pa = a.params[i];
        const auto& pb = b.params[i];
        if (pa.annotated_type != pb.annotated_type) return false;
        if (pa.is_rest != pb.is_rest) return false;
        if (pa.is_destructure != pb.is_destructure) return false;
        if (required(pa) != required(pb)) return false;
    }
    return true;
}

DefineFunctionResult SymbolTable::define_function(const UserFunctionInfo& func_info) {
    SymbolId id = interner_->intern(func_info.name);
    auto& current = scopes_.back();
    auto it = current.find(id);
    if (it != current.end() && it->second.kind == SymbolKind::UserFunction) {
        auto& overloads = it->second.overloads;
        // A user-source definition shadows the stdlib base layer: the first
        // user `fn name` replaces ALL stdlib overloads of that name (the
        // documented "user code can shadow stdlib" idiom). Later user defs then
        // accumulate among themselves by signature. (Stdlib defs are processed
        // first, so a stdlib def never meets a pre-existing user set.)
        if (!func_info.is_stdlib) {
            bool all_stdlib = true;
            for (const auto& uf : overloads) {
                if (!uf.is_stdlib) { all_stdlib = false; break; }
            }
            if (all_stdlib) {
                overloads.clear();
                overloads.push_back(func_info);
                return DefineFunctionResult::Added;
            }
        }
        // Accumulate into the existing overload set: a matching signature
        // replaces in place (last-wins), a new signature appends.
        for (auto& uf : overloads) {
            if (same_signature(uf, func_info)) {
                uf = func_info;
                return DefineFunctionResult::ReplacedSameSignature;
            }
        }
        overloads.push_back(func_info);
        return DefineFunctionResult::Accumulated;
    }
    // First definition of this name in the current scope (or it shadows a
    // non-function symbol of the same name — replace it with a fresh set).
    Symbol sym{};
    sym.kind = SymbolKind::UserFunction;
    sym.name_id = id;
    sym.name = func_info.name;
    sym.buffer_index = 0xFFFF;  // Not applicable for functions
    sym.overloads.push_back(func_info);
    current.insert_or_assign(id, sym);
    return DefineFunctionResult::Added;
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
    if (id != NULL_SYMBOL) return lookup(id);
    // Phase 3: a builtin name the source never mentions is not interned,
    // but by-name lookup must still resolve it (pre-Phase-3 every builtin
    // was interned at construction, so find() always succeeded).
    if (use_builtins_) return lookup_builtin(name, NULL_SYMBOL);
    return std::nullopt;
}

std::optional<Symbol> SymbolTable::lookup(SymbolId id) const {
    if (id == NULL_SYMBOL) return std::nullopt;
    // Search from innermost scope outward — user definitions shadow builtins.
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(id);
        if (found != it->end()) {
            return found->second;
        }
    }
    // Phase 3: fall back to the process-shared builtin scope.
    if (use_builtins_ && interner_) return lookup_builtin(interner_->view(id), id);
    return std::nullopt;
}

bool SymbolTable::is_defined_in_current_scope(std::string_view name) const {
    if (scopes_.empty() || !interner_) return false;
    // Phase 3: builtins used to be materialized in scope 0, which made their
    // names count as "defined" at global scope (E150 forbids `sin = 5` at
    // top level but allows shadowing inside a closure). Preserve that.
    if (use_builtins_ && scopes_.size() == 1 && lookup_builtin(name, NULL_SYMBOL)) {
        return true;
    }
    SymbolId id = interner_->find(name);
    if (id == NULL_SYMBOL) return false;
    return scopes_.back().find(id) != scopes_.back().end();
}

static void update_symbol_nodes(Symbol& sym, const std::unordered_map<NodeIndex, NodeIndex>& node_map) {
    if (sym.kind == SymbolKind::UserFunction) {
        // Phase 4: remap every overload — each carries its own body/def nodes
        // and per-param default nodes that all moved during the AST clone.
        for (auto& uf : sym.overloads) {
            auto body_it = node_map.find(uf.body_node);
            if (body_it != node_map.end()) uf.body_node = body_it->second;
            auto def_it = node_map.find(uf.def_node);
            if (def_it != node_map.end()) uf.def_node = def_it->second;
            for (auto& param : uf.params) {
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
