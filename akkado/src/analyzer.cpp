#include "akkado/analyzer.hpp"
#include "akkado/builtins.hpp"
#include "akkado/source_map.hpp"
#include <unordered_set>

namespace akkado {

AnalysisResult SemanticAnalyzer::analyze(const Ast& ast, std::string_view filename,
                                          const SourceMap* source_map,
                                          std::span<const ModuleNamespace> namespaces) {
    input_ast_ = &ast;
    output_arena_ = AstArena{};
    symbols_ = SymbolTable{};
    diagnostics_.clear();
    node_map_.clear();
    filename_ = std::string(filename);
    source_map_ = source_map;
    namespaces_.assign(namespaces.begin(), namespaces.end());

    if (!ast.valid()) {
        error("E001", "Invalid AST: no root node", {});
        return {std::move(symbols_), {}, std::move(diagnostics_), false};
    }

    // Pass 1: Collect variable definitions
    collect_definitions(ast.root);

    // Pass 1.5: Hide definitions from namespaced modules
    hide_namespaced_definitions();

    // Pass 2: Rewrite pipes (builds new AST)
    NodeIndex new_root = rewrite_pipes(ast.root);

    // Pass 2.5: Update function body nodes to point to transformed AST
    update_function_body_nodes();

    // Pass 3: Resolve and validate function calls
    resolve_and_validate(new_root);

    bool success = !has_errors(diagnostics_);

    AnalysisResult result;
    result.symbols = std::move(symbols_);
    result.transformed_ast.arena = std::move(output_arena_);
    result.transformed_ast.root = new_root;
    result.diagnostics = std::move(diagnostics_);
    result.success = success;

    return result;
}

// Check if a call node has _ placeholders at a given position
static bool is_placeholder_node(const AstArena& arena, NodeIndex child) {
    const Node& cn = arena[child];
    if (cn.type == NodeType::Identifier &&
        std::holds_alternative<Node::IdentifierData>(cn.data) &&
        cn.as_identifier() == "_") {
        return true;
    }
    if (cn.type == NodeType::Argument && cn.first_child != NULL_NODE) {
        const Node& inner = arena[cn.first_child];
        if (inner.type == NodeType::Identifier &&
            std::holds_alternative<Node::IdentifierData>(inner.data) &&
            inner.as_identifier() == "_") {
            return true;
        }
    }
    return false;
}

// Check if all _ placeholders in a call are at positions with defaults.
// Returns true if this should be default-filling (not partial application).
static bool all_placeholders_have_defaults(const AstArena& arena,
                                            const Node& call_node,
                                            const SymbolTable& symbols) {
    if (!std::holds_alternative<Node::IdentifierData>(call_node.data)) {
        return false;
    }
    const std::string& func_name = call_node.as_identifier();

    const BuiltinInfo* builtin = lookup_builtin(func_name);

    const UserFunctionInfo* user_func = nullptr;
    if (!builtin) {
        auto sym = symbols.lookup(func_name);
        if (sym && sym->kind == SymbolKind::UserFunction) {
            user_func = &sym->user_function;
        }
    }

    if (!builtin && !user_func) {
        return false;  // Unknown function — assume partial application
    }

    std::size_t arg_idx = 0;
    NodeIndex child = call_node.first_child;
    while (child != NULL_NODE) {
        if (is_placeholder_node(arena, child)) {
            if (builtin) {
                if (!builtin->has_default(arg_idx)) return false;
            } else {
                if (arg_idx >= user_func->params.size()) return false;
                const auto& param = user_func->params[arg_idx];
                bool has_default = param.default_value.has_value() ||
                                   param.default_node != NULL_NODE ||
                                   param.default_string.has_value();
                if (!has_default) return false;
            }
        }
        ++arg_idx;
        child = arena[child].next_sibling;
    }
    return true;
}

std::string SemanticAnalyzer::extract_definition_name(const Node& n) {
    if (n.type == NodeType::Assignment || n.type == NodeType::ConstDecl) {
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            return n.as_identifier();
        }
    }
    if (n.type == NodeType::FunctionDef) {
        if (std::holds_alternative<Node::FunctionDefData>(n.data)) {
            return n.as_function_def().name;
        }
    }
    return {};
}

void SemanticAnalyzer::hide_namespaced_definitions() {
    if (namespaces_.empty() || !source_map_) return;

    // Build map: canonical_path → alias
    std::unordered_map<std::string, std::string> path_to_alias;
    // Build set of directly-imported canonical paths (those NOT in namespaces)
    // We need to compare against all module source regions that are NOT namespaced
    std::unordered_set<std::string> namespaced_paths;
    for (const auto& ns : namespaces_) {
        path_to_alias[ns.canonical_path] = ns.alias;
        namespaced_paths.insert(ns.canonical_path);
    }

    // Walk top-level AST children
    const Node& root = input_ast_->arena[input_ast_->root];
    NodeIndex child_idx = root.first_child;
    while (child_idx != NULL_NODE) {
        const Node& child = input_ast_->arena[child_idx];
        std::string def_name = extract_definition_name(child);

        if (!def_name.empty()) {
            // Determine which module this node comes from
            const auto* region = source_map_->find_region(child.location.offset);
            if (region) {
                auto alias_it = path_to_alias.find(region->filename);
                if (alias_it != path_to_alias.end()) {
                    const std::string& alias = alias_it->second;

                    // Look up the existing symbol to copy it under qualified name
                    auto sym = symbols_.lookup(def_name);
                    if (sym) {
                        // Register qualified name (e.g., "f.lp")
                        Symbol qsym = *sym;
                        std::string qname = alias + "." + def_name;
                        qsym.name_hash = fnv1a_hash(qname);
                        qsym.name = qname;
                        qsym.origin_module = region->filename;
                        symbols_.define(qsym);

                        // Hide the original (unqualified) symbol
                        symbols_.hide_symbol(def_name, region->filename);
                    }
                }
            }
        }
        child_idx = child.next_sibling;
    }

    // Register Module symbols for each alias
    for (const auto& ns : namespaces_) {
        symbols_.define_module(ns.alias, ns.canonical_path);
    }
}

void SemanticAnalyzer::collect_definitions(NodeIndex node) {
    if (node == NULL_NODE) return;

    const Node& n = (*input_ast_).arena[node];

    if (n.type == NodeType::Assignment) {
        // Variable name is stored in the node's data (as IdentifierData)
        const std::string& name = n.as_identifier();
        // Check if RHS is a pattern expression (MiniLiteral)
        NodeIndex rhs = n.first_child;

        // Builtin variable assignment (bpm, sr) — skip normal registration
        {
            auto bv_it = BUILTIN_VARIABLES.find(name);
            if (bv_it != BUILTIN_VARIABLES.end()) {
                if (bv_it->second.setter_name.empty()) {
                    error("E170", "Cannot assign to read-only builtin variable '" + name + "'", n.location);
                }
                // Skip variable registration — codegen handles the desugaring
                // Still recurse into children for nested definitions
                NodeIndex child = n.first_child;
                while (child != NULL_NODE) {
                    collect_definitions(child);
                    child = (*input_ast_).arena[child].next_sibling;
                }
                return;
            }
        }

        // Immutability check: error if variable already defined in current scope
        if (symbols_.is_defined_in_current_scope(name)) {
            error("E150", "Cannot reassign immutable variable '" + name + "'", n.location);
            // Continue processing to collect all errors
        }

        if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::MiniLiteral) {
            // Pattern assignment
            PatternInfo pat_info{};
            pat_info.pattern_node = rhs;  // Will be updated after AST transform
            pat_info.is_sample_pattern = false;
            symbols_.define_pattern(name, pat_info);
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::ArrayLit) {
            // Array assignment - count elements and register as Array symbol
            ArrayInfo arr_info{};
            arr_info.source_node = rhs;
            arr_info.element_count = 0;
            NodeIndex elem = (*input_ast_).arena[rhs].first_child;
            while (elem != NULL_NODE) {
                arr_info.element_count++;
                elem = (*input_ast_).arena[elem].next_sibling;
            }
            symbols_.define_array(name, arr_info);
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::RecordLit) {
            // Record assignment - collect field names and register as Record symbol
            auto record_type = std::make_shared<RecordTypeInfo>();
            record_type->source_node = rhs;

            // Check for spread source: {..base, ...}
            const Node& rec_node = (*input_ast_).arena[rhs];
            if (std::holds_alternative<Node::RecordLitData>(rec_node.data)) {
                const auto& rec_data = rec_node.as_record_lit();
                if (rec_data.spread_source != NULL_NODE) {
                    // Look up the spread source's record type
                    const Node& spread_node = (*input_ast_).arena[rec_data.spread_source];
                    if (spread_node.type == NodeType::Identifier) {
                        std::string spread_name;
                        if (std::holds_alternative<Node::IdentifierData>(spread_node.data)) {
                            spread_name = spread_node.as_identifier();
                        }
                        auto spread_sym = symbols_.lookup(spread_name);
                        if (spread_sym && spread_sym->kind == SymbolKind::Record && spread_sym->record_type) {
                            // Copy fields from spread source
                            for (const auto& f : spread_sym->record_type->fields) {
                                record_type->fields.push_back(f);
                            }
                        }
                    } else if (spread_node.type == NodeType::RecordLit) {
                        // Inline record literal as spread source — collect its fields
                        NodeIndex sf = spread_node.first_child;
                        while (sf != NULL_NODE) {
                            const Node& sfield = (*input_ast_).arena[sf];
                            if (sfield.type == NodeType::Argument &&
                                std::holds_alternative<Node::RecordFieldData>(sfield.data)) {
                                RecordFieldInfo field_info;
                                field_info.name = sfield.as_record_field().name;
                                field_info.buffer_index = 0xFFFF;
                                field_info.field_kind = SymbolKind::Variable;
                                record_type->fields.push_back(std::move(field_info));
                            }
                            sf = (*input_ast_).arena[sf].next_sibling;
                        }
                    }
                }
            }

            // Collect explicit field names from RecordLit children (each is an Argument with RecordFieldData)
            // These override any spread fields with the same name
            NodeIndex field_node = (*input_ast_).arena[rhs].first_child;
            while (field_node != NULL_NODE) {
                const Node& field = (*input_ast_).arena[field_node];
                if (field.type == NodeType::Argument &&
                    std::holds_alternative<Node::RecordFieldData>(field.data)) {
                    const auto& field_data = field.as_record_field();
                    // Check if this field was already added from spread
                    bool found = false;
                    for (auto& existing : record_type->fields) {
                        if (existing.name == field_data.name) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        RecordFieldInfo field_info;
                        field_info.name = field_data.name;
                        field_info.buffer_index = 0xFFFF;
                        field_info.field_kind = SymbolKind::Variable;
                        record_type->fields.push_back(std::move(field_info));
                    }
                }
                field_node = (*input_ast_).arena[field_node].next_sibling;
            }

            symbols_.define_record(name, record_type);
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::Closure) {
            // Lambda assignment - register as FunctionValue
            FunctionRef func_ref{};
            func_ref.closure_node = rhs;
            func_ref.is_user_function = false;
            // Extract parameters from closure
            NodeIndex child = (*input_ast_).arena[rhs].first_child;
            while (child != NULL_NODE) {
                const Node& child_node = (*input_ast_).arena[child];
                if (child_node.type == NodeType::Identifier) {
                    FunctionParamInfo param;
                    if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                        const auto& cp = child_node.as_closure_param();
                        param.name = cp.name;
                        param.default_value = cp.default_value;
                        param.default_string = cp.default_string;
                        param.is_rest = cp.is_rest;
                        // Expression default is stored as child of param node
                        if (child_node.first_child != NULL_NODE &&
                            !cp.default_value.has_value() && !cp.default_string.has_value()) {
                            param.default_node = child_node.first_child;
                        }
                    } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                        param.name = child_node.as_identifier();
                        param.default_value = std::nullopt;
                    } else {
                        break;  // Not a parameter, must be body
                    }
                    func_ref.params.push_back(std::move(param));
                } else {
                    break;  // Body node
                }
                child = (*input_ast_).arena[child].next_sibling;
            }
            symbols_.define_function_value(name, func_ref);
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::Pipe) {
            // Check for pipe-to-lambda: x |> f(%) |> g(%)
            // If innermost LHS of pipe chain is an unbound identifier, this becomes a closure
            // Walk down nested pipes to find the innermost LHS
            NodeIndex current = rhs;
            while (current != NULL_NODE && (*input_ast_).arena[current].type == NodeType::Pipe) {
                current = (*input_ast_).arena[current].first_child;
            }

            // current is now the innermost LHS (should be Identifier for pipe-to-lambda)
            if (current != NULL_NODE) {
                const Node& lhs_node = (*input_ast_).arena[current];
                if (lhs_node.type == NodeType::Identifier) {
                    std::string param_name;
                    if (std::holds_alternative<Node::IdentifierData>(lhs_node.data)) {
                        param_name = lhs_node.as_identifier();
                    }
                    // Check if identifier is unbound (not defined)
                    if (!param_name.empty() && !symbols_.lookup(param_name)) {
                        // This is pipe-to-lambda: register as FunctionValue
                        FunctionRef func_ref{};
                        func_ref.closure_node = rhs;  // Will be updated to the closure
                        func_ref.is_user_function = false;
                        FunctionParamInfo param;
                        param.name = param_name;
                        param.default_value = std::nullopt;
                        func_ref.params.push_back(std::move(param));
                        symbols_.define_function_value(name, func_ref);
                    } else {
                        // LHS is bound - regular variable
                        symbols_.define_variable(name, 0xFFFF);
                    }
                } else {
                    // LHS is not an identifier - regular variable
                    symbols_.define_variable(name, 0xFFFF);
                }
            } else {
                // Invalid pipe - regular variable
                symbols_.define_variable(name, 0xFFFF);
            }
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::Call) {
            // Check if this is a partial application (has _ placeholder args)
            bool has_placeholder = false;
            {
                NodeIndex child = (*input_ast_).arena[rhs].first_child;
                while (child != NULL_NODE) {
                    const Node& cn = (*input_ast_).arena[child];
                    if (cn.type == NodeType::Identifier &&
                        std::holds_alternative<Node::IdentifierData>(cn.data) &&
                        cn.as_identifier() == "_") {
                        has_placeholder = true;
                        break;
                    }
                    if (cn.type == NodeType::Argument && cn.first_child != NULL_NODE) {
                        const Node& inner = (*input_ast_).arena[cn.first_child];
                        if (inner.type == NodeType::Identifier &&
                            std::holds_alternative<Node::IdentifierData>(inner.data) &&
                            inner.as_identifier() == "_") {
                            has_placeholder = true;
                            break;
                        }
                    }
                    child = (*input_ast_).arena[child].next_sibling;
                }
            }
            if (has_placeholder &&
                !all_placeholders_have_defaults((*input_ast_).arena,
                                                 (*input_ast_).arena[rhs], symbols_)) {
                // Partial application: will be rewritten to a closure during pipe rewriting
                // Count placeholder params
                std::size_t placeholder_count = 0;
                NodeIndex child = (*input_ast_).arena[rhs].first_child;
                while (child != NULL_NODE) {
                    const Node& cn = (*input_ast_).arena[child];
                    bool is_ph = false;
                    if (cn.type == NodeType::Identifier &&
                        std::holds_alternative<Node::IdentifierData>(cn.data) &&
                        cn.as_identifier() == "_") is_ph = true;
                    if (cn.type == NodeType::Argument && cn.first_child != NULL_NODE) {
                        const Node& inner = (*input_ast_).arena[cn.first_child];
                        if (inner.type == NodeType::Identifier &&
                            std::holds_alternative<Node::IdentifierData>(inner.data) &&
                            inner.as_identifier() == "_") is_ph = true;
                    }
                    if (is_ph) placeholder_count++;
                    child = (*input_ast_).arena[child].next_sibling;
                }
                FunctionRef func_ref{};
                func_ref.closure_node = rhs;  // Will be updated after rewrite
                func_ref.is_user_function = false;
                for (std::size_t i = 0; i < placeholder_count; ++i) {
                    FunctionParamInfo param;
                    param.name = "__p" + std::to_string(i);
                    func_ref.params.push_back(std::move(param));
                }
                symbols_.define_function_value(name, func_ref);
            } else {
                // Check if calling a function that returns a closure
                const Node& call_node = (*input_ast_).arena[rhs];
                std::string callee_name;
                if (std::holds_alternative<Node::IdentifierData>(call_node.data)) {
                    callee_name = call_node.as_identifier();
                }
                auto callee_sym = symbols_.lookup(callee_name);
                if (callee_sym && callee_sym->kind == SymbolKind::UserFunction) {
                    NodeIndex body = callee_sym->user_function.body_node;
                    if (body != NULL_NODE && (*input_ast_).arena[body].type == NodeType::Closure) {
                        FunctionRef func_ref{};
                        func_ref.closure_node = body;
                        func_ref.is_user_function = false;
                        const Node& closure_body = (*input_ast_).arena[body];
                        NodeIndex child = closure_body.first_child;
                        while (child != NULL_NODE) {
                            const Node& child_node = (*input_ast_).arena[child];
                            if (child_node.type == NodeType::Identifier) {
                                FunctionParamInfo param;
                                if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                                    const auto& cp = child_node.as_closure_param();
                                    param.name = cp.name;
                                    param.default_value = cp.default_value;
                                    param.default_string = cp.default_string;
                                    param.is_rest = cp.is_rest;
                                    // Expression default is stored as child of param node
                                    if (child_node.first_child != NULL_NODE &&
                                        !cp.default_value.has_value() && !cp.default_string.has_value()) {
                                        param.default_node = child_node.first_child;
                                    }
                                } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                                    param.name = child_node.as_identifier();
                                } else {
                                    break;
                                }
                                func_ref.params.push_back(std::move(param));
                            } else {
                                break;
                            }
                            child = (*input_ast_).arena[child].next_sibling;
                        }
                        symbols_.define_function_value(name, func_ref);
                    } else {
                        symbols_.define_variable(name, 0xFFFF);
                    }
                } else if (callee_name == "compose") {
                    // compose() returns a function value
                    FunctionRef func_ref{};
                    func_ref.is_user_function = false;
                    symbols_.define_function_value(name, func_ref);
                } else if (callee_name == "state") {
                    // Phase 4b: `v = state({...})` produces a state cell.
                    // Mark the binding so FieldAccess validation (E061) skips
                    // its non-record check and lets codegen route the
                    // `cell.field` read sugar instead. Whether the cell holds
                    // a record vs a scalar is settled at codegen time; this
                    // flag just opens the door past the analyzer's gate.
                    Symbol sym{};
                    sym.kind = SymbolKind::Variable;
                    sym.name_hash = fnv1a_hash(name);
                    sym.name = name;
                    sym.buffer_index = 0xFFFF;
                    sym.is_state_cell = true;
                    symbols_.define(sym);
                } else {
                    symbols_.define_variable(name, 0xFFFF);
                }
            }
        } else if (rhs != NULL_NODE && (*input_ast_).arena[rhs].type == NodeType::Identifier) {
            // Phase 4b: alias propagation — `t = v` where `v` is a state cell.
            // The new binding inherits the cell-handle semantics so `t.field`
            // works just like `v.field`.
            std::string rhs_name;
            if (std::holds_alternative<Node::IdentifierData>(
                    (*input_ast_).arena[rhs].data)) {
                rhs_name = (*input_ast_).arena[rhs].as_identifier();
            }
            auto rhs_sym = !rhs_name.empty() ? symbols_.lookup(rhs_name)
                                              : std::nullopt;
            if (rhs_sym && rhs_sym->is_state_cell) {
                Symbol sym{};
                sym.kind = SymbolKind::Variable;
                sym.name_hash = fnv1a_hash(name);
                sym.name = name;
                sym.buffer_index = 0xFFFF;
                sym.is_state_cell = true;
                symbols_.define(sym);
            } else {
                symbols_.define_variable(name, 0xFFFF);
            }
        } else {
            // Regular variable assignment
            symbols_.define_variable(name, 0xFFFF);
        }
    }

    if (n.type == NodeType::DestructureAssignment) {
        // Statement-level destructure: {x, y} = expr
        // Register each field name as an immutable variable so identifier
        // resolution during pipe-rewrite (closure detection) and codegen
        // both find the binding. The actual buffer index is allocated when
        // codegen visits the RHS and runs bind_destructure_fields().
        const auto& dd = n.as_destructure_assignment();
        for (const auto& field : dd.fields) {
            if (symbols_.is_defined_in_current_scope(field.name)) {
                error("E150", "Cannot reassign immutable variable '" + field.name + "'", n.location);
            } else {
                symbols_.define_variable(field.name, 0xFFFF);
            }
        }
    }

    if (n.type == NodeType::ConstDecl) {
        // Const variable declaration: const x = expr
        const std::string& name = n.as_identifier();

        // Cannot declare builtin variable as const
        {
            auto bv_it = BUILTIN_VARIABLES.find(name);
            if (bv_it != BUILTIN_VARIABLES.end()) {
                error("E170", "Cannot declare builtin variable '" + name + "' as const", n.location);
                return;
            }
        }

        if (symbols_.is_defined_in_current_scope(name)) {
            error("E150", "Cannot reassign immutable variable '" + name + "'", n.location);
        }

        // Register as a const placeholder (value not yet known; will be set during codegen)
        symbols_.define_const_placeholder(name);
    }

    if (n.type == NodeType::FunctionDef) {
        // Register the user-defined function
        const auto& fn_data = n.as_function_def();

        if (symbols_.is_defined_in_current_scope(fn_data.name)) {
            warning("Function '" + fn_data.name + "' redefined", n.location);
        }

        // Collect parameters from Identifier children (before body)
        UserFunctionInfo func_info;
        func_info.name = fn_data.name;
        func_info.def_node = node;
        func_info.has_rest_param = fn_data.has_rest_param;
        func_info.is_const = fn_data.is_const;

        NodeIndex child = n.first_child;
        std::size_t param_idx = 0;
        while (child != NULL_NODE && param_idx < fn_data.param_count) {
            const Node& child_node = (*input_ast_).arena[child];
            FunctionParamInfo param;
            if (child_node.type == NodeType::DestructureParam) {
                // Phase 3b: `fn f({x, y [= default]})`. Synthetic name; the
                // actual fields drive per-field local bindings at call time.
                const auto& dp = child_node.as_destructure_param();
                param.name = "__destr_param_" + std::to_string(param_idx);
                param.is_destructure = true;
                param.destructure_fields = dp.fields;
            } else if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                const auto& cp = child_node.as_closure_param();
                param.name = cp.name;
                param.default_value = cp.default_value;
                param.default_string = cp.default_string;
                param.is_rest = cp.is_rest;
                // Expression default is stored as child of param node
                if (child_node.first_child != NULL_NODE &&
                    !cp.default_value.has_value() && !cp.default_string.has_value()) {
                    param.default_node = child_node.first_child;
                }
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param.name = child_node.as_identifier();
                param.default_value = std::nullopt;
            }
            func_info.params.push_back(std::move(param));
            param_idx++;
            child = (*input_ast_).arena[child].next_sibling;
        }

        // Body is the next child after params
        func_info.body_node = child;

        // Check if body is explicitly a Closure (for closure-return detection in codegen)
        if (child != NULL_NODE && (*input_ast_).arena[child].type == NodeType::Closure) {
            func_info.returns_closure = true;
        }

        symbols_.define_function(func_info);
    }

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        collect_definitions(child);
        child = (*input_ast_).arena[child].next_sibling;
    }
}

void SemanticAnalyzer::update_function_body_nodes() {
    // Update all user function definitions to point to the transformed AST
    symbols_.update_function_nodes(node_map_);
}

NodeIndex SemanticAnalyzer::rewrite_pipes(NodeIndex node) {
    if (node == NULL_NODE) return NULL_NODE;

    const Node& n = (*input_ast_).arena[node];

    if (n.type == NodeType::Pipe) {
        // Pipe: LHS |> RHS
        // Get LHS (first child) and RHS (second child)
        NodeIndex lhs_idx = n.first_child;
        NodeIndex rhs_idx = (lhs_idx != NULL_NODE) ?
                            (*input_ast_).arena[lhs_idx].next_sibling : NULL_NODE;

        if (lhs_idx == NULL_NODE || rhs_idx == NULL_NODE) {
            error("E002", "Invalid pipe expression", n.location);
            return NULL_NODE;
        }

        // Handle PipeBinding: expr as name |> ...
        // The binding should be visible in ALL subsequent pipe stages,
        // not just the immediate RHS. For left-associative pipe chains like
        // ((A as r) |> B) |> C, we need to find the PipeBinding by walking
        // the leftmost spine of nested pipes.
        std::string binding_name;
        NodeIndex binding_value = NULL_NODE;  // original bound expression (for substitution)
        NodeIndex actual_lhs = lhs_idx;
        std::vector<std::string> destructure_fields;

        const Node& lhs_node = (*input_ast_).arena[lhs_idx];
        if (lhs_node.type == NodeType::PipeBinding) {
            const auto& binding_data = lhs_node.as_pipe_binding();
            binding_name = binding_data.binding_name;
            destructure_fields = binding_data.destructure_fields;
            actual_lhs = lhs_node.first_child;
            binding_value = actual_lhs;  // the expression being bound
        } else {
            // Walk down the leftmost spine of nested pipes to find a PipeBinding
            NodeIndex walk = lhs_idx;
            while (walk != NULL_NODE && (*input_ast_).arena[walk].type == NodeType::Pipe) {
                NodeIndex inner_lhs = (*input_ast_).arena[walk].first_child;
                if (inner_lhs != NULL_NODE &&
                    (*input_ast_).arena[inner_lhs].type == NodeType::PipeBinding) {
                    const auto& bd = (*input_ast_).arena[inner_lhs].as_pipe_binding();
                    binding_name = bd.binding_name;
                    destructure_fields = bd.destructure_fields;
                    binding_value = (*input_ast_).arena[inner_lhs].first_child;
                    break;
                }
                walk = inner_lhs;
            }
        }

        // Check if innermost LHS is an unbound identifier -> create closure
        // Walk down nested pipes to find the innermost LHS
        NodeIndex innermost_lhs = actual_lhs;
        while (innermost_lhs != NULL_NODE &&
               (*input_ast_).arena[innermost_lhs].type == NodeType::Pipe) {
            innermost_lhs = (*input_ast_).arena[innermost_lhs].first_child;
        }

        if (innermost_lhs != NULL_NODE) {
            const Node& lhs = (*input_ast_).arena[innermost_lhs];
            if (lhs.type == NodeType::Identifier) {
                std::string name;
                if (std::holds_alternative<Node::IdentifierData>(lhs.data)) {
                    name = lhs.as_identifier();
                }
                if (!name.empty() && !symbols_.lookup(name)) {
                    // Unbound identifier - transform to closure: x |> f(%) -> (x) -> f(x)
                    // For the closure, we use the innermost identifier as param
                    // and the whole pipe chain (except innermost) as body
                    NodeIndex closure = create_closure_from_pipe(innermost_lhs, node, n.location);
                    node_map_[node] = closure;
                    return closure;
                }
            }
        }

        // First, recursively rewrite the actual LHS (may contain nested pipes)
        NodeIndex new_lhs = rewrite_pipes(actual_lhs);

        // Now substitute all holes in RHS with the LHS value
        // This performs: a |> f(%) -> f(a)
        // Also substitute binding name references with the LHS value
        NodeIndex new_rhs;
        if (!binding_name.empty()) {
            // binding_value is an input AST node — clone_subtree in substitute_nodes
            // will correctly clone it from input→output arena each time r is referenced.
            // clone_on_hole must match whether PipeBinding is directly on LHS:
            // - Direct binding: clone_on_hole=true (original behavior)
            // - Inherited from inner pipe: clone_on_hole=false (replacement is already output arena)
            bool direct_binding = (lhs_node.type == NodeType::PipeBinding);
            SubstituteOpts opts;
            opts.replacement = new_lhs;
            opts.binding_name = binding_name;
            opts.clone_on_hole = direct_binding;
            opts.binding_replacement = binding_value;
            opts.destructure_fields = destructure_fields;
            new_rhs = substitute_nodes(rhs_idx, opts);
        } else {
            new_rhs = substitute_nodes(rhs_idx, {new_lhs});
        }

        // Track mapping so function/closure bodies pointing to this pipe get updated
        node_map_[node] = new_rhs;

        // The pipe is eliminated - return the transformed RHS
        return new_rhs;
    }

    // For non-pipe nodes, clone and recurse
    return clone_subtree(node);
}

NodeIndex SemanticAnalyzer::clone_node(NodeIndex src_idx) {
    if (src_idx == NULL_NODE) return NULL_NODE;

    const Node& src = (*input_ast_).arena[src_idx];

    // Allocate in output arena
    NodeIndex dst_idx = output_arena_.alloc(src.type, src.location);
    Node& dst = output_arena_[dst_idx];

    // Copy data
    dst.data = src.data;

    // Track mapping for substitute_holes
    node_map_[src_idx] = dst_idx;

    return dst_idx;
}

NodeIndex SemanticAnalyzer::clone_subtree(NodeIndex src_idx) {
    if (src_idx == NULL_NODE) return NULL_NODE;

    // Check if already cloned
    auto it = node_map_.find(src_idx);
    if (it != node_map_.end()) {
        return it->second;
    }

    const Node& src = (*input_ast_).arena[src_idx];

    // Handle pipe nodes specially during cloning
    if (src.type == NodeType::Pipe) {
        return rewrite_pipes(src_idx);
    }

    // Desugar method calls: receiver.method(a, b) -> method(receiver, a, b)
    if (src.type == NodeType::MethodCall) {
        return desugar_method_call(src_idx);
    }

    // Rewrite partial application: f(a, _, b) -> (p0) -> f(a, p0, b)
    if (src.type == NodeType::Call) {
        // Check if any argument is Identifier("_")
        bool has_placeholder = false;
        NodeIndex child = src.first_child;
        while (child != NULL_NODE) {
            const Node& cn = (*input_ast_).arena[child];
            // Check direct Identifier("_")
            if (cn.type == NodeType::Identifier &&
                std::holds_alternative<Node::IdentifierData>(cn.data) &&
                cn.as_identifier() == "_") {
                has_placeholder = true;
                break;
            }
            // Check Argument wrapping Identifier("_")
            if (cn.type == NodeType::Argument && cn.first_child != NULL_NODE) {
                const Node& inner = (*input_ast_).arena[cn.first_child];
                if (inner.type == NodeType::Identifier &&
                    std::holds_alternative<Node::IdentifierData>(inner.data) &&
                    inner.as_identifier() == "_") {
                    has_placeholder = true;
                    break;
                }
            }
            child = (*input_ast_).arena[child].next_sibling;
        }

        if (has_placeholder &&
            !all_placeholders_have_defaults((*input_ast_).arena, src, symbols_)) {
            // Partial application: Build closure replacing each _ with a generated param name
            // 1. Clone the call, replacing _ with generated param names
            NodeIndex cloned_call = clone_node(src_idx);
            std::vector<std::string> param_names;
            NodeIndex src_child = src.first_child;
            NodeIndex prev_dst = NULL_NODE;

            while (src_child != NULL_NODE) {
                const Node& sc = (*input_ast_).arena[src_child];
                NodeIndex dst_child;

                // Check if this argument is _ (directly or wrapped in Argument)
                bool is_placeholder = false;
                if (sc.type == NodeType::Identifier &&
                    std::holds_alternative<Node::IdentifierData>(sc.data) &&
                    sc.as_identifier() == "_") {
                    is_placeholder = true;
                }
                if (sc.type == NodeType::Argument && sc.first_child != NULL_NODE) {
                    const Node& inner = (*input_ast_).arena[sc.first_child];
                    if (inner.type == NodeType::Identifier &&
                        std::holds_alternative<Node::IdentifierData>(inner.data) &&
                        inner.as_identifier() == "_") {
                        is_placeholder = true;
                    }
                }

                if (is_placeholder) {
                    std::string pname = "__p" + std::to_string(param_names.size());
                    param_names.push_back(pname);
                    // Create identifier node with generated param name
                    dst_child = output_arena_.alloc(NodeType::Identifier, sc.location);
                    output_arena_[dst_child].data = Node::IdentifierData{pname};
                } else {
                    dst_child = clone_subtree(src_child);
                }

                if (dst_child != NULL_NODE) {
                    output_arena_[dst_child].next_sibling = NULL_NODE;
                    if (prev_dst == NULL_NODE) {
                        output_arena_[cloned_call].first_child = dst_child;
                    } else {
                        output_arena_[prev_dst].next_sibling = dst_child;
                    }
                    prev_dst = dst_child;
                }
                src_child = (*input_ast_).arena[src_child].next_sibling;
            }

            // 2. Build closure: (p0, p1, ...) -> cloned_call
            NodeIndex closure = output_arena_.alloc(NodeType::Closure, src.location);

            // Add param nodes as children, then body (cloned_call)
            NodeIndex prev = NULL_NODE;
            for (const auto& pname : param_names) {
                NodeIndex param_node = output_arena_.alloc(NodeType::Identifier, src.location);
                output_arena_[param_node].data = Node::IdentifierData{pname};
                output_arena_[param_node].next_sibling = NULL_NODE;
                if (prev == NULL_NODE) {
                    output_arena_[closure].first_child = param_node;
                } else {
                    output_arena_[prev].next_sibling = param_node;
                }
                prev = param_node;
            }
            // Append the call as the closure body
            output_arena_[cloned_call].next_sibling = NULL_NODE;
            if (prev == NULL_NODE) {
                output_arena_[closure].first_child = cloned_call;
            } else {
                output_arena_[prev].next_sibling = cloned_call;
            }

            node_map_[src_idx] = closure;
            return closure;
        }
    }

    // Clone this node
    NodeIndex dst_idx = clone_node(src_idx);

    // Handle MatchArm guard_node specially - it's not a child but stored in data
    if (src.type == NodeType::MatchArm) {
        const auto& arm_data = src.as_match_arm();
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            // Clone the guard expression
            NodeIndex cloned_guard = clone_subtree(arm_data.guard_node);
            // Update the MatchArmData in the cloned node
            auto& dst_data = std::get<Node::MatchArmData>(output_arena_[dst_idx].data);
            dst_data.guard_node = cloned_guard;
        }
    }

    // Handle RecordLit spread_source - it's stored in data, not as a child
    if (src.type == NodeType::RecordLit &&
        std::holds_alternative<Node::RecordLitData>(src.data)) {
        const auto& rec_data = src.as_record_lit();
        if (rec_data.spread_source != NULL_NODE) {
            NodeIndex cloned_spread = clone_subtree(rec_data.spread_source);
            auto& dst_data = std::get<Node::RecordLitData>(output_arena_[dst_idx].data);
            dst_data.spread_source = cloned_spread;
        }
    }

    // Handle Argument spread_source — `..expr` arg or array element. The
    // spread expression is hung on ArgumentData.spread_source rather than as
    // a child, so it must be cloned/remapped here too.
    if (src.type == NodeType::Argument &&
        std::holds_alternative<Node::ArgumentData>(src.data)) {
        const auto& arg_data = src.as_argument();
        if (arg_data.spread_source != NULL_NODE) {
            NodeIndex cloned_spread = clone_subtree(arg_data.spread_source);
            auto& dst_data = std::get<Node::ArgumentData>(output_arena_[dst_idx].data);
            dst_data.spread_source = cloned_spread;
        }
    }

    // Phase 3b: destructure default expressions ride inside the data variant
    // (one NodeIndex per field), not as child links. Clone each into the
    // output arena and patch the cloned data so codegen sees valid indices.
    if (src.type == NodeType::DestructureAssignment &&
        std::holds_alternative<Node::DestructureAssignmentData>(src.data)) {
        const auto& dd_src = src.as_destructure_assignment();
        auto& dst_data = std::get<Node::DestructureAssignmentData>(output_arena_[dst_idx].data);
        for (std::size_t i = 0; i < dd_src.fields.size(); ++i) {
            if (dd_src.fields[i].default_node != NULL_NODE) {
                dst_data.fields[i].default_node =
                    clone_subtree(dd_src.fields[i].default_node);
            }
        }
    }
    if (src.type == NodeType::DestructureParam &&
        std::holds_alternative<Node::DestructureParamData>(src.data)) {
        const auto& dp_src = src.as_destructure_param();
        auto& dst_data = std::get<Node::DestructureParamData>(output_arena_[dst_idx].data);
        for (std::size_t i = 0; i < dp_src.fields.size(); ++i) {
            if (dp_src.fields[i].default_node != NULL_NODE) {
                dst_data.fields[i].default_node =
                    clone_subtree(dp_src.fields[i].default_node);
            }
        }
    }

    // Clone children
    NodeIndex src_child = src.first_child;
    NodeIndex prev_dst_child = NULL_NODE;

    while (src_child != NULL_NODE) {
        NodeIndex dst_child = clone_subtree(src_child);

        if (dst_child != NULL_NODE) {
            if (prev_dst_child == NULL_NODE) {
                output_arena_[dst_idx].first_child = dst_child;
            } else {
                output_arena_[prev_dst_child].next_sibling = dst_child;
            }
            prev_dst_child = dst_child;
        }

        src_child = (*input_ast_).arena[src_child].next_sibling;
    }

    return dst_idx;
}

NodeIndex SemanticAnalyzer::substitute_nodes(NodeIndex node, const SubstituteOpts& opts) {
    if (node == NULL_NODE) return NULL_NODE;

    const Node& n = (*input_ast_).arena[node];

    // 1. Hole → replace with replacement (clone if clone_on_hole)
    if (n.type == NodeType::Hole) {
        // Check for hole with field access: %.field
        if (std::holds_alternative<Node::HoleData>(n.data)) {
            const auto& hole_data = n.as_hole();
            if (hole_data.field_name.has_value()) {
                NodeIndex base = opts.clone_on_hole
                    ? clone_subtree(opts.replacement)
                    : opts.replacement;
                NodeIndex field_access = output_arena_.alloc(NodeType::FieldAccess, n.location);
                output_arena_[field_access].data = Node::FieldAccessData{hole_data.field_name.value()};
                output_arena_[field_access].first_child = base;
                return field_access;
            }
        }
        return opts.clone_on_hole ? clone_subtree(opts.replacement) : opts.replacement;
    }

    // 2. Identifier matching binding_name → clone binding_replacement (or replacement as fallback)
    if (!opts.binding_name.empty() && n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        }
        if (name == opts.binding_name) {
            NodeIndex bind_repl = (opts.binding_replacement != NULL_NODE)
                ? opts.binding_replacement : opts.replacement;
            return clone_subtree(bind_repl);
        }
    }

    // 2b. Identifier matching a destructure field → rewrite to binding.field access
    if (!opts.destructure_fields.empty() && n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        }
        for (const auto& field : opts.destructure_fields) {
            if (name == field) {
                // Rewrite `field` → `__destr_N.field` (FieldAccess on the binding)
                NodeIndex bind_repl = (opts.binding_replacement != NULL_NODE)
                    ? opts.binding_replacement : opts.replacement;
                NodeIndex base = clone_subtree(bind_repl);
                NodeIndex field_access = output_arena_.alloc(NodeType::FieldAccess, n.location);
                output_arena_[field_access].data = Node::FieldAccessData{field};
                output_arena_[field_access].first_child = base;
                return field_access;
            }
        }
    }

    // 3. Node identity match → return replacement directly
    if (opts.identifier_to_replace != NULL_NODE && node == opts.identifier_to_replace) {
        return opts.replacement;
    }

    // 4. Pipe → recurse LHS, then RHS with adjusted opts
    if (n.type == NodeType::Pipe) {
        NodeIndex src_lhs = n.first_child;
        NodeIndex src_rhs = (src_lhs != NULL_NODE) ?
                            (*input_ast_).arena[src_lhs].next_sibling : NULL_NODE;

        NodeIndex new_lhs = substitute_nodes(src_lhs, opts);

        // For RHS: replacement becomes new_lhs, and drop identifier_to_replace
        // (standard pipe semantics: only holes in RHS get the LHS value)
        // But binding_name carries through (as-binding is visible in entire chain)
        // binding_replacement stays constant — it always refers to the original bound value
        SubstituteOpts rhs_opts;
        rhs_opts.replacement = new_lhs;
        rhs_opts.binding_name = opts.binding_name;
        rhs_opts.binding_replacement = (opts.binding_replacement != NULL_NODE)
            ? opts.binding_replacement : opts.replacement;
        rhs_opts.identifier_to_replace = NULL_NODE;  // drop identity match for RHS
        rhs_opts.clone_on_hole = opts.clone_on_hole;
        rhs_opts.destructure_fields = opts.destructure_fields;

        NodeIndex new_rhs = substitute_nodes(src_rhs, rhs_opts);

        node_map_[node] = new_rhs;
        return new_rhs;
    }

    // 5. Clone node, recurse children
    NodeIndex new_node = clone_node(node);

    // Handle MatchArm guard_node specially
    if (n.type == NodeType::MatchArm) {
        const auto& arm_data = n.as_match_arm();
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            NodeIndex new_guard = substitute_nodes(arm_data.guard_node, opts);
            auto& dst_data = std::get<Node::MatchArmData>(output_arena_[new_node].data);
            dst_data.guard_node = new_guard;
        }
    }

    // Handle RecordLit spread_source specially
    if (n.type == NodeType::RecordLit &&
        std::holds_alternative<Node::RecordLitData>(n.data)) {
        const auto& rec_data = n.as_record_lit();
        if (rec_data.spread_source != NULL_NODE) {
            NodeIndex new_spread = substitute_nodes(rec_data.spread_source, opts);
            auto& dst_data = std::get<Node::RecordLitData>(output_arena_[new_node].data);
            dst_data.spread_source = new_spread;
        }
    }

    // Handle Argument spread_source specially (..expr arg or array element).
    if (n.type == NodeType::Argument &&
        std::holds_alternative<Node::ArgumentData>(n.data)) {
        const auto& arg_data = n.as_argument();
        if (arg_data.spread_source != NULL_NODE) {
            NodeIndex new_spread = substitute_nodes(arg_data.spread_source, opts);
            auto& dst_data = std::get<Node::ArgumentData>(output_arena_[new_node].data);
            dst_data.spread_source = new_spread;
        }
    }

    NodeIndex src_child = n.first_child;
    NodeIndex prev_dst_child = NULL_NODE;

    while (src_child != NULL_NODE) {
        NodeIndex dst_child = substitute_nodes(src_child, opts);

        if (dst_child != NULL_NODE) {
            if (prev_dst_child == NULL_NODE) {
                output_arena_[new_node].first_child = dst_child;
            } else {
                output_arena_[prev_dst_child].next_sibling = dst_child;
            }
            prev_dst_child = dst_child;
        }

        src_child = (*input_ast_).arena[src_child].next_sibling;
    }

    // 6. Desugar MethodCall → Call (inlined desugar_method_call_output)
    if (n.type == NodeType::MethodCall) {
        Node& src_out = output_arena_[new_node];

        NodeIndex call_idx = output_arena_.alloc(NodeType::Call, src_out.location);
        output_arena_[call_idx].data = src_out.data;

        NodeIndex receiver = src_out.first_child;
        if (receiver == NULL_NODE) {
            error("E008", "Method call missing receiver", src_out.location);
            return call_idx;
        }

        NodeIndex receiver_arg = output_arena_.alloc(NodeType::Argument, src_out.location);
        output_arena_[receiver_arg].data = Node::ArgumentData{std::nullopt};
        output_arena_[receiver_arg].first_child = receiver;

        NodeIndex remaining_args = output_arena_[receiver].next_sibling;
        output_arena_[receiver].next_sibling = NULL_NODE;

        output_arena_[call_idx].first_child = receiver_arg;
        output_arena_[receiver_arg].next_sibling = remaining_args;

        return call_idx;
    }

    return new_node;
}

bool SemanticAnalyzer::contains_hole(NodeIndex node) const {
    if (node == NULL_NODE) return false;

    const Node& n = (*input_ast_).arena[node];

    if (n.type == NodeType::Hole) return true;

    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        if (contains_hole(child)) return true;
        child = (*input_ast_).arena[child].next_sibling;
    }

    return false;
}



NodeIndex SemanticAnalyzer::create_closure_from_pipe(
    NodeIndex param_node, NodeIndex body_node, SourceLocation loc) {

    const Node& param_src = (*input_ast_).arena[param_node];
    const std::string& param_name = param_src.as_identifier();

    // Create closure node
    NodeIndex closure = output_arena_.alloc(NodeType::Closure, loc);

    // Clone parameter as closure param
    NodeIndex param = clone_node(param_node);
    output_arena_[closure].first_child = param;

    // Push scope, define param
    symbols_.push_scope();
    symbols_.define_variable(param_name, 0xFFFF);

    // Create param reference for hole substitution
    NodeIndex param_ref = output_arena_.alloc(NodeType::Identifier, loc);
    output_arena_[param_ref].data = Node::IdentifierData{param_name};

    // Substitute holes AND the parameter identifier in body with param reference
    NodeIndex body = substitute_nodes(body_node, {param_ref, {}, param_node});
    output_arena_[param].next_sibling = body;

    symbols_.pop_scope();

    return closure;
}

NodeIndex SemanticAnalyzer::desugar_method_call(NodeIndex method_call_idx) {
    // Desugar: receiver.method(a, b) -> method(receiver, a, b)
    const Node& src = (*input_ast_).arena[method_call_idx];
    const std::string& method_name = src.as_identifier();

    // Get the receiver (first child of MethodCall)
    NodeIndex src_receiver = src.first_child;
    if (src_receiver == NULL_NODE) {
        NodeIndex call_idx = output_arena_.alloc(NodeType::Call, src.location);
        output_arena_[call_idx].data = src.data;
        node_map_[method_call_idx] = call_idx;
        error("E008", "Method call missing receiver", src.location);
        return call_idx;
    }

    // Check if receiver is a Module identifier (namespace-qualified call)
    const Node& receiver_node = (*input_ast_).arena[src_receiver];
    if (receiver_node.type == NodeType::Identifier) {
        std::string receiver_name;
        if (std::holds_alternative<Node::IdentifierData>(receiver_node.data)) {
            receiver_name = receiver_node.as_identifier();
        }
        auto sym = symbols_.lookup(receiver_name);
        if (sym && sym->kind == SymbolKind::Module) {
            // Module-qualified call: f.lp(sig, 800) → Call "f.lp"(sig, 800)
            auto mod_sym = symbols_.lookup_in_module(sym->module_path, method_name);
            if (!mod_sym) {
                error("E504", "Module '" + receiver_name + "' has no definition '" + method_name + "'", src.location);
            }

            std::string qname = receiver_name + "." + method_name;
            NodeIndex call_idx = output_arena_.alloc(NodeType::Call, src.location);
            output_arena_[call_idx].data = Node::IdentifierData{qname};
            node_map_[method_call_idx] = call_idx;

            // Clone arguments (skip receiver — it's the module, not a signal)
            NodeIndex src_arg = (*input_ast_).arena[src_receiver].next_sibling;
            NodeIndex prev_dst_arg = NULL_NODE;
            while (src_arg != NULL_NODE) {
                NodeIndex dst_arg = clone_subtree(src_arg);
                if (dst_arg != NULL_NODE) {
                    if (prev_dst_arg == NULL_NODE) {
                        output_arena_[call_idx].first_child = dst_arg;
                    } else {
                        output_arena_[prev_dst_arg].next_sibling = dst_arg;
                    }
                    prev_dst_arg = dst_arg;
                }
                src_arg = (*input_ast_).arena[src_arg].next_sibling;
            }
            return call_idx;
        }
    }

    // Standard method call desugaring: receiver.method(a, b) -> method(receiver, a, b)
    NodeIndex call_idx = output_arena_.alloc(NodeType::Call, src.location);
    output_arena_[call_idx].data = src.data;  // Copy the method name (IdentifierData)

    // Track mapping
    node_map_[method_call_idx] = call_idx;

    // Clone the receiver and wrap it in an Argument node
    NodeIndex cloned_receiver = clone_subtree(src_receiver);
    NodeIndex receiver_arg = output_arena_.alloc(NodeType::Argument, src.location);
    output_arena_[receiver_arg].data = Node::ArgumentData{std::nullopt};  // Positional arg
    output_arena_[receiver_arg].first_child = cloned_receiver;

    // Set receiver as first child of the Call
    output_arena_[call_idx].first_child = receiver_arg;

    // Clone remaining arguments (after the receiver)
    NodeIndex src_arg = (*input_ast_).arena[src_receiver].next_sibling;
    NodeIndex prev_dst_arg = receiver_arg;

    while (src_arg != NULL_NODE) {
        NodeIndex dst_arg = clone_subtree(src_arg);
        if (dst_arg != NULL_NODE) {
            output_arena_[prev_dst_arg].next_sibling = dst_arg;
            prev_dst_arg = dst_arg;
        }
        src_arg = (*input_ast_).arena[src_arg].next_sibling;
    }

    return call_idx;
}


void SemanticAnalyzer::resolve_and_validate(NodeIndex node) {
    if (node == NULL_NODE) return;

    const Node& n = output_arena_[node];

    if (n.type == NodeType::Hole) {
        // Holes should have been substituted - if we see one, it's an error
        error("E003", "Hole '%' used outside of pipe expression", n.location);
    }

    if (n.type == NodeType::Call) {
        // Function name is stored in the node's data (as IdentifierData)
        const std::string& func_name = n.as_identifier();

        // Look up in symbol table
        auto sym = symbols_.lookup(func_name);
        if (!sym) {
            error("E004", "Unknown function: '" + func_name + "'", n.location);
        } else if (sym->kind == SymbolKind::UserFunction) {
            // Validate user function call
            const auto& fn = sym->user_function;

            // Spread arguments are statically unsized — defer arg-count and
            // reorder validation to codegen, which expands ..record / ..array
            // sources into concrete args via expand_call_arguments().
            const bool spread = has_spread_arg(node);

            if (!spread) {
                // Reorder named arguments if any
                std::vector<std::string> pnames;
                for (const auto& p : fn.params) pnames.push_back(p.name);
                reorder_named_arguments(node, pnames, func_name);

                // Count arguments
                std::size_t arg_count = 0;
                NodeIndex arg = output_arena_[node].first_child;
                while (arg != NULL_NODE) {
                    arg_count++;
                    arg = output_arena_[arg].next_sibling;
                }

                // Count required args (params without defaults or rest)
                std::size_t min_args = 0;
                for (const auto& param : fn.params) {
                    if (!param.default_value.has_value() &&
                        !param.default_string.has_value() &&
                        param.default_node == NULL_NODE &&
                        !param.is_rest) {
                        min_args++;
                    }
                }

                if (arg_count < min_args) {
                    error("E006", "Function '" + func_name + "' expects at least " +
                          std::to_string(min_args) + " argument(s), got " +
                          std::to_string(arg_count), n.location);
                } else if (!fn.has_rest_param) {
                    // Only enforce max if no rest param
                    std::size_t max_args = fn.params.size();
                    if (arg_count > max_args) {
                        error("E007", "Function '" + func_name + "' expects at most " +
                              std::to_string(max_args) + " argument(s), got " +
                              std::to_string(arg_count), n.location);
                    }
                }
            }
        } else if (sym->kind == SymbolKind::Builtin) {
            // Spread arguments are statically unsized — defer validation to
            // codegen, which expands ..record / ..array sources before the
            // builtin handler runs (Phase 6).
            if (has_spread_arg(node)) {
                // Skip arg-count and reorder for spread calls.
            } else
            if (func_name == "tap_delay" || func_name == "tap_delay_ms" || func_name == "tap_delay_smp") {
                // Validate tap_delay(in, time, fb, processor, [dry], [wet])
                std::size_t arg_count = 0;
                NodeIndex arg = output_arena_[node].first_child;
                NodeIndex fourth_arg = NULL_NODE;
                while (arg != NULL_NODE) {
                    arg_count++;
                    if (arg_count == 4) {
                        const Node& arg_node = output_arena_[arg];
                        fourth_arg = (arg_node.type == NodeType::Argument) ?
                                     arg_node.first_child : arg;
                    }
                    arg = output_arena_[arg].next_sibling;
                }
                if (arg_count < 4 || arg_count > 6) {
                    error("E301", "Function '" + func_name + "' expects 4-6 arguments: "
                          + func_name + "(in, time, fb, processor, [dry], [wet])", n.location);
                } else if (fourth_arg != NULL_NODE) {
                    // Validate 4th argument is a closure with 1 parameter
                    const Node& proc_node = output_arena_[fourth_arg];
                    if (proc_node.type != NodeType::Closure) {
                        error("E302", "tap_delay() 4th argument must be a closure: (x) -> ...",
                              proc_node.location);
                    } else {
                        // Count closure parameters
                        int param_count = 0;
                        NodeIndex child = proc_node.first_child;
                        while (child != NULL_NODE) {
                            const Node& child_node = output_arena_[child];
                            if (child_node.type == NodeType::Identifier &&
                                (std::holds_alternative<Node::ClosureParamData>(child_node.data) ||
                                 std::holds_alternative<Node::IdentifierData>(child_node.data))) {
                                param_count++;
                            } else {
                                break;  // Body node
                            }
                            child = output_arena_[child].next_sibling;
                        }
                        if (param_count != 1) {
                            error("E303", "tap_delay() processor closure must have exactly 1 parameter",
                                  proc_node.location);
                        }
                    }
                }
            } else if (func_name == "compose") {
                // compose() requires at least 2 function arguments, no upper limit
                std::size_t arg_count = 0;
                NodeIndex arg = output_arena_[node].first_child;
                while (arg != NULL_NODE) {
                    arg_count++;
                    arg = output_arena_[arg].next_sibling;
                }
                if (arg_count < 2) {
                    error("E006", "Function 'compose' expects at least 2 arguments", n.location);
                }
            } else if (func_name == "sum") {
                // sum() is variadic: sum(array) or sum(a, b, ...), arity >= 1.
                std::size_t arg_count = 0;
                NodeIndex arg = output_arena_[node].first_child;
                while (arg != NULL_NODE) {
                    arg_count++;
                    arg = output_arena_[arg].next_sibling;
                }
                if (arg_count < 1) {
                    error("E006", "Function 'sum' expects at least 1 argument", n.location);
                }
            } else {
                // Reorder named arguments if any
                reorder_named_arguments(node, sym->builtin, func_name);

                // Count arguments (children of the Call node)
                std::size_t arg_count = 0;
                NodeIndex arg = output_arena_[node].first_child;
                while (arg != NULL_NODE) {
                    arg_count++;
                    arg = output_arena_[arg].next_sibling;
                }

                validate_arguments(func_name, sym->builtin, arg_count, n.location);
            }
        }
    }

    if (n.type == NodeType::MatchExpr) {
        // Validate match expression
        // NOTE: Literal scrutinee check is done in codegen (after inline expansion)

        // Check if this is a scrutinee form or guard-only form
        bool has_scrutinee = false;
        if (std::holds_alternative<Node::MatchExprData>(n.data)) {
            has_scrutinee = n.as_match_expr().has_scrutinee;
        }

        // Determine first arm based on whether scrutinee exists
        NodeIndex first_arm = n.first_child;
        if (has_scrutinee && first_arm != NULL_NODE) {
            first_arm = output_arena_[first_arm].next_sibling;
        }

        // Check for duplicate patterns and unreachable code
        std::set<std::string> seen_patterns;
        bool seen_wildcard = false;
        NodeIndex arm = first_arm;

        while (arm != NULL_NODE) {
            const Node& arm_node = output_arena_[arm];
            if (arm_node.type == NodeType::MatchArm) {
                const auto& arm_data = arm_node.as_match_arm();

                if (seen_wildcard) {
                    warning("Unreachable pattern after wildcard '_'", arm_node.location);
                }

                if (arm_data.is_wildcard) {
                    seen_wildcard = true;
                } else if (!arm_data.has_guard) {
                    // Only check for duplicates on arms without guards
                    // Guards make the same pattern semantically different
                    NodeIndex pattern = arm_node.first_child;
                    if (pattern != NULL_NODE) {
                        const Node& pattern_node = output_arena_[pattern];
                        std::string pattern_key;

                        if (pattern_node.type == NodeType::StringLit) {
                            pattern_key = "s:" + pattern_node.as_string();
                        } else if (pattern_node.type == NodeType::NumberLit) {
                            pattern_key = "n:" + std::to_string(pattern_node.as_number());
                        } else if (pattern_node.type == NodeType::BoolLit) {
                            pattern_key = "b:" + std::to_string(pattern_node.as_bool());
                        }

                        if (!pattern_key.empty()) {
                            if (seen_patterns.count(pattern_key)) {
                                warning("Duplicate pattern in match expression", pattern_node.location);
                            }
                            seen_patterns.insert(pattern_key);
                        }
                    }
                }
            }
            arm = output_arena_[arm].next_sibling;
        }
    }

    if (n.type == NodeType::ConstDecl) {
        // Validate const declaration RHS
        NodeIndex rhs = n.first_child;
        if (rhs != NULL_NODE) {
            // Verify purity of the RHS expression
            verify_const_purity(rhs, "const variable");
            resolve_and_validate(rhs);
        }
        return;
    }

    if (n.type == NodeType::FunctionDef) {
        // Validate function definition: check no outer variable captures
        const auto& fn_data = n.as_function_def();
        std::set<std::string> params;

        // Push a new scope for function parameters
        symbols_.push_scope();

        // Collect parameter names
        NodeIndex child = n.first_child;
        std::size_t param_idx = 0;

        while (child != NULL_NODE && param_idx < fn_data.param_count) {
            const Node& child_node = output_arena_[child];
            std::string param_name;
            if (child_node.type == NodeType::DestructureParam) {
                // Phase 3b: each destructured field is bound as a parameter
                // so the body can reference it by name. The synthetic slot
                // name `__destr_param_<N>` is intentionally NOT registered
                // — users can't reference it.
                const auto& dp = child_node.as_destructure_param();
                for (const auto& field : dp.fields) {
                    if (params.count(field.name)) {
                        error("E188",
                              "Duplicate parameter name '" + field.name +
                              "' in destructure pattern", child_node.location);
                    } else {
                        params.insert(field.name);
                        symbols_.define_parameter(field.name, 0xFFFF);
                    }
                }
            } else if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_name = child_node.as_closure_param().name;
                // Verify purity of expression defaults
                const auto& cp = child_node.as_closure_param();
                if (child_node.first_child != NULL_NODE &&
                    !cp.default_value.has_value() && !cp.default_string.has_value()) {
                    verify_const_purity(child_node.first_child,
                        "default expression for parameter '" + param_name + "'");
                }
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_name = child_node.as_identifier();
            }
            if (!param_name.empty()) {
                params.insert(param_name);
                symbols_.define_parameter(param_name, 0xFFFF);
            }
            param_idx++;
            child = output_arena_[child].next_sibling;
        }

        // Check body for captured variables (body is after params)
        NodeIndex body = child;
        if (body != NULL_NODE) {
            check_closure_captures(body, params, n.location);

            // Verify purity for const fn
            if (fn_data.is_const) {
                verify_const_purity(body, "const fn '" + fn_data.name + "'");
            }
        }

        // Recurse to children while params are in scope
        child = n.first_child;
        while (child != NULL_NODE) {
            resolve_and_validate(child);
            child = output_arena_[child].next_sibling;
        }

        // Pop scope
        symbols_.pop_scope();

        return;  // Already recursed
    }

    if (n.type == NodeType::Identifier) {
        // Check if identifier is defined
        // Note: Identifier nodes may use IdentifierData or ClosureParamData (for params with defaults)
        std::string name;
        if (std::holds_alternative<Node::ClosureParamData>(n.data)) {
            name = n.as_closure_param().name;
        } else if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        } else {
            // Shouldn't happen - unknown data type for Identifier node
            return;
        }
        // Skip validation for _ placeholder — handled by codegen in call arguments
        if (name != "_") {
            auto sym = symbols_.lookup(name);
            if (!sym) {
                // Builtin variables (bpm, sr) are not in the symbol table —
                // they're desugared to ENV_GET in codegen
                if (BUILTIN_VARIABLES.find(name) == BUILTIN_VARIABLES.end()) {
                    error("E005", "Undefined identifier: '" + name + "'", n.location);
                }
            }
        }
        // FunctionValue and UserFunction can be used as values
        // (already allowed by symbol table lookup)
    }

    if (n.type == NodeType::FieldAccess) {
        // Validate field access: check that field exists on the record type
        const auto& field_data = n.as_field_access();
        const std::string& field_name = field_data.field_name;

        // Get the expression being accessed (first child)
        NodeIndex expr = n.first_child;
        if (expr != NULL_NODE) {
            // First, validate the expression
            resolve_and_validate(expr);

            // Try to determine the type of the expression and validate field access
            // For MVP, we only validate field access on direct identifier references to records
            const Node& expr_node = output_arena_[expr];
            if (expr_node.type == NodeType::Identifier) {
                std::string expr_name;
                if (std::holds_alternative<Node::IdentifierData>(expr_node.data)) {
                    expr_name = expr_node.as_identifier();
                }
                if (!expr_name.empty()) {
                    auto sym = symbols_.lookup(expr_name);
                    if (sym && sym->kind == SymbolKind::Module) {
                        // Module-qualified field access (e.g., f.pi)
                        auto mod_sym = symbols_.lookup_in_module(sym->module_path, field_name);
                        if (!mod_sym) {
                            error("E504", "Module '" + expr_name + "' has no definition '" + field_name + "'", n.location);
                        }
                        return;  // Don't recurse into module identifier
                    } else if (sym && sym->kind == SymbolKind::Record && sym->record_type) {
                        // Check if field exists
                        const auto* field = sym->record_type->find_field(field_name);
                        if (!field) {
                            // Build list of available fields for error message
                            std::string available;
                            auto field_names = sym->record_type->field_names();
                            for (size_t i = 0; i < field_names.size(); ++i) {
                                if (i > 0) available += ", ";
                                available += field_names[i];
                            }
                            error("E060", "Unknown field '" + field_name + "' on record. Available: " + available, n.location);
                        }
                    } else if (sym && sym->kind != SymbolKind::Record
                             && sym->kind != SymbolKind::Pattern
                             && sym->kind != SymbolKind::Parameter
                             && sym->kind != SymbolKind::Module
                             && !sym->is_state_cell) {
                        // Phase 4b: state-cell variables surface field access
                        // through codegen's read sugar; skip the analyzer's
                        // pre-flight rejection for them. Codegen still emits
                        // E135 if the cell turns out to be scalar.
                        error("E061", "Cannot access field '" + field_name + "' on non-record value", n.location);
                    }
                }
            }
            // For nested field access (e.g., a.b.c) or complex expressions,
            // we skip validation for MVP - codegen will catch errors
        }
        return;  // Already validated child
    }

    if (n.type == NodeType::PipeBinding) {
        // Pipe binding creates a scoped symbol for the bound name
        const auto& binding_data = n.as_pipe_binding();
        const std::string& binding_name = binding_data.binding_name;

        // Define the binding in current scope (it will be visible in subsequent pipe stages)
        // The actual type will be determined based on the expression being bound
        NodeIndex bound_expr = n.first_child;
        if (bound_expr != NULL_NODE) {
            resolve_and_validate(bound_expr);

            // Try to determine the type of the bound expression and propagate it
            const Node& expr_node = output_arena_[bound_expr];
            if (expr_node.type == NodeType::Identifier) {
                std::string expr_name;
                if (std::holds_alternative<Node::IdentifierData>(expr_node.data)) {
                    expr_name = expr_node.as_identifier();
                }
                auto sym = symbols_.lookup(expr_name);
                if (sym) {
                    // Create a new symbol for the binding with the same type
                    if (sym->kind == SymbolKind::Record && sym->record_type) {
                        symbols_.define_record(binding_name, sym->record_type);
                    } else {
                        symbols_.define_variable(binding_name, 0xFFFF);
                    }
                } else {
                    symbols_.define_variable(binding_name, 0xFFFF);
                }
            } else if (expr_node.type == NodeType::RecordLit) {
                // Inline record literal - create record type from it
                auto record_type = std::make_shared<RecordTypeInfo>();
                record_type->source_node = bound_expr;

                // Check for spread source: {..base, ...}
                if (std::holds_alternative<Node::RecordLitData>(expr_node.data)) {
                    const auto& rec_data = expr_node.as_record_lit();
                    if (rec_data.spread_source != NULL_NODE) {
                        const Node& spread_node = output_arena_[rec_data.spread_source];
                        if (spread_node.type == NodeType::Identifier) {
                            std::string spread_name;
                            if (std::holds_alternative<Node::IdentifierData>(spread_node.data)) {
                                spread_name = spread_node.as_identifier();
                            }
                            auto spread_sym = symbols_.lookup(spread_name);
                            if (spread_sym && spread_sym->kind == SymbolKind::Record && spread_sym->record_type) {
                                for (const auto& f : spread_sym->record_type->fields) {
                                    record_type->fields.push_back(f);
                                }
                            }
                        } else if (spread_node.type == NodeType::RecordLit) {
                            NodeIndex sf = spread_node.first_child;
                            while (sf != NULL_NODE) {
                                const Node& sfield = output_arena_[sf];
                                if (sfield.type == NodeType::Argument &&
                                    std::holds_alternative<Node::RecordFieldData>(sfield.data)) {
                                    RecordFieldInfo field_info;
                                    field_info.name = sfield.as_record_field().name;
                                    field_info.buffer_index = 0xFFFF;
                                    field_info.field_kind = SymbolKind::Variable;
                                    record_type->fields.push_back(std::move(field_info));
                                }
                                sf = output_arena_[sf].next_sibling;
                            }
                        }
                    }
                }

                // Collect explicit field names (override spread fields with same name)
                NodeIndex field_node = expr_node.first_child;
                while (field_node != NULL_NODE) {
                    const Node& field = output_arena_[field_node];
                    if (field.type == NodeType::Argument &&
                        std::holds_alternative<Node::RecordFieldData>(field.data)) {
                        const auto& field_data = field.as_record_field();
                        bool found = false;
                        for (auto& existing : record_type->fields) {
                            if (existing.name == field_data.name) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            RecordFieldInfo field_info;
                            field_info.name = field_data.name;
                            field_info.buffer_index = 0xFFFF;
                            field_info.field_kind = SymbolKind::Variable;
                            record_type->fields.push_back(std::move(field_info));
                        }
                    }
                    field_node = output_arena_[field_node].next_sibling;
                }
                symbols_.define_record(binding_name, record_type);
            } else if (expr_node.type == NodeType::MiniLiteral) {
                // Pattern literal bound to name - define as Pattern for field access
                PatternInfo pat_info{};
                pat_info.pattern_node = bound_expr;
                pat_info.is_sample_pattern = false;
                symbols_.define_pattern(binding_name, pat_info);
            } else if (expr_node.type == NodeType::Call) {
                // Check if this is a pattern-producing call (chord, pat, etc.)
                std::string call_name;
                if (std::holds_alternative<Node::IdentifierData>(expr_node.data)) {
                    call_name = expr_node.as_identifier();
                }
                if (call_name == "chord" || call_name == "pat" || call_name == "seq" ||
                    call_name == "value" || call_name == "note") {
                    // Pattern-producing call - define as Pattern for field access
                    PatternInfo pat_info{};
                    pat_info.pattern_node = bound_expr;
                    pat_info.is_sample_pattern = false;
                    symbols_.define_pattern(binding_name, pat_info);
                } else {
                    symbols_.define_variable(binding_name, 0xFFFF);
                }
            } else {
                // For other expression types, treat as a simple variable
                symbols_.define_variable(binding_name, 0xFFFF);
            }
        }
        return;  // Already validated child
    }

    if (n.type == NodeType::Hole) {
        // Check for hole with field access (%.field)
        if (std::holds_alternative<Node::HoleData>(n.data)) {
            const auto& hole_data = n.as_hole();
            if (hole_data.field_name.has_value()) {
                // This is %.field - validation happens at the pipe level
                // For MVP, we just note this and let codegen handle it
                // A proper implementation would check that we're in a pattern pipe context
            }
        }
        // Regular holes are validated elsewhere (E003 for holes outside pipes)
    }

    if (n.type == NodeType::Closure) {
        // Validate closure: collect parameters, then check body for captures
        std::set<std::string> params;

        // Push a new scope for closure parameters
        symbols_.push_scope();

        // Collect parameter names (Identifier children before body)
        // Parameters may be stored as IdentifierData or ClosureParamData
        NodeIndex child = n.first_child;
        NodeIndex body = NULL_NODE;

        while (child != NULL_NODE) {
            const Node& child_node = output_arena_[child];
            if (child_node.type == NodeType::Identifier) {
                // Check if it's IdentifierData or ClosureParamData
                std::string param_name;
                if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                    param_name = child_node.as_closure_param().name;
                } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                    param_name = child_node.as_identifier();
                } else {
                    // This is the body (not a parameter)
                    body = child;
                    break;
                }
                params.insert(param_name);
                // Define parameter in current scope
                symbols_.define_variable(param_name, 0xFFFF);
            } else {
                // This is the body
                body = child;
                break;
            }
            child = child_node.next_sibling;
        }

        // Check body for captured variables
        if (body != NULL_NODE) {
            check_closure_captures(body, params, n.location);
        }

        // Recurse to children (including body) while params are in scope
        child = n.first_child;
        while (child != NULL_NODE) {
            resolve_and_validate(child);
            child = output_arena_[child].next_sibling;
        }

        // Pop scope - parameters go out of scope
        symbols_.pop_scope();

        return;  // Already recursed, don't do it again below
    }

    // Special handling for MatchArm: skip pattern, validate guard and body
    // For destructuring arms, push scope with field bindings
    if (n.type == NodeType::MatchArm) {
        const auto& arm_data = n.as_match_arm();

        // Destructuring arms: define field names in a new scope
        if (arm_data.is_destructure && !arm_data.destructure_fields.empty()) {
            symbols_.push_scope();
            for (const auto& field : arm_data.destructure_fields) {
                symbols_.define_variable(field, 0xFFFF);
            }
        }

        // Validate guard expression if present
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            resolve_and_validate(arm_data.guard_node);
        }

        // Validate body
        NodeIndex pattern = n.first_child;
        if (pattern != NULL_NODE) {
            NodeIndex body = output_arena_[pattern].next_sibling;
            if (body != NULL_NODE) {
                resolve_and_validate(body);
            }
        }

        if (arm_data.is_destructure && !arm_data.destructure_fields.empty()) {
            symbols_.pop_scope();
        }
        return;
    }

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        resolve_and_validate(child);
        child = output_arena_[child].next_sibling;
    }
}

void SemanticAnalyzer::validate_arguments(const std::string& func_name,
                                          const BuiltinInfo& builtin,
                                          std::size_t arg_count,
                                          SourceLocation loc) {
    std::size_t min_args = builtin.input_count;
    std::size_t max_args = builtin.input_count + builtin.optional_count
                         + builtin.extended_param_count;

    if (arg_count < min_args) {
        error("E006", "Function '" + func_name + "' expects at least " +
              std::to_string(min_args) + " argument(s), got " +
              std::to_string(arg_count), loc);
    } else if (arg_count > max_args) {
        error("E007", "Function '" + func_name + "' expects at most " +
              std::to_string(max_args) + " argument(s), got " +
              std::to_string(arg_count), loc);
    }
}

void SemanticAnalyzer::error(const std::string& message, SourceLocation loc) {
    error("E000", message, loc);
}

void SemanticAnalyzer::error(const std::string& code, const std::string& message,
                             SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Error;
    diag.code = code;
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

void SemanticAnalyzer::warning(const std::string& message, SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Warning;
    diag.code = "W000";
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

void SemanticAnalyzer::verify_const_purity(NodeIndex node, const std::string& context) {
    if (node == NULL_NODE) return;

    const Node& n = output_arena_[node];

    if (n.type == NodeType::Call) {
        const std::string& func_name = n.as_identifier();

        // Whitelist of pure functions allowed in const context
        static const std::set<std::string> pure_builtins = {
            // Arithmetic (binary op desugaring)
            "add", "sub", "mul", "div", "pow",
            // Comparison (binary op desugaring: ==, !=, <, >, <=, >=)
            "eq", "neq", "lt", "gt", "lte", "gte",
            // Boolean (op desugaring: &&, ||, !)
            "band", "bor", "bnot",
            // Math
            "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
            "sqrt", "abs", "log", "log10", "exp", "exp2",
            "floor", "ceil", "round", "trunc", "fract",
            "min", "max", "clamp", "sign",
            // Conversion
            "mtof",
            // Array operations (compile-time)
            "range", "map", "sum", "len",
            "linspace", "harmonics", "random",
            "mean",
            // Logic
            "select",
        };

        auto sym = symbols_.lookup(func_name);
        if (sym) {
            if (sym->kind == SymbolKind::Builtin) {
                if (pure_builtins.find(func_name) == pure_builtins.end()) {
                    error("E202", "Non-pure function '" + func_name +
                          "' cannot be used in " + context, n.location);
                    return;
                }
            } else if (sym->kind == SymbolKind::UserFunction) {
                if (!sym->user_function.is_const) {
                    error("E202", "Non-const function '" + func_name +
                          "' cannot be used in " + context, n.location);
                    return;
                }
            }
        }
    }

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        verify_const_purity(child, context);
        child = output_arena_[child].next_sibling;
    }
}

void SemanticAnalyzer::check_closure_captures(NodeIndex node,
                                               const std::set<std::string>& params,
                                               SourceLocation closure_loc) {
    if (node == NULL_NODE) return;

    const Node& n = output_arena_[node];

    // Skip match arm patterns - they are not variable references
    if (n.type == NodeType::MatchArm) {
        const auto& arm_data = n.as_match_arm();

        // Check guard expression if present
        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
            check_closure_captures(arm_data.guard_node, params, closure_loc);
        }

        // Only check the body (second child), not the pattern (first child)
        NodeIndex pattern = n.first_child;
        if (pattern != NULL_NODE) {
            NodeIndex body = output_arena_[pattern].next_sibling;
            if (body != NULL_NODE) {
                check_closure_captures(body, params, closure_loc);
            }
        }
        return;
    }

    if (n.type == NodeType::Identifier) {
        // Get name - may be IdentifierData or ClosureParamData (for params with defaults)
        std::string name;
        if (std::holds_alternative<Node::ClosureParamData>(n.data)) {
            name = n.as_closure_param().name;
        } else if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        } else {
            return;  // Unknown data type
        }

        // Check if it's a parameter
        if (params.find(name) != params.end()) {
            return;  // OK - parameter reference
        }

        // Check if it's a builtin, user function, or function value
        auto sym = symbols_.lookup(name);
        if (sym && (sym->kind == SymbolKind::Builtin ||
                    sym->kind == SymbolKind::UserFunction ||
                    sym->kind == SymbolKind::FunctionValue)) {
            return;  // OK - builtin, user function, or function value
        }

        // It's a captured variable - allowed for closures (read-only capture)
        // Variables are immutable, so read-only capture is safe
        if (sym && (sym->kind == SymbolKind::Variable ||
                    sym->kind == SymbolKind::Parameter ||
                    sym->kind == SymbolKind::Array)) {
            return;  // OK - captured variable (will be bound at codegen time)
        }

        // Unknown identifier - will be caught elsewhere
        return;
    }

    // For Call nodes, the function name is in data, not as a child
    // So we don't need special handling - just check children

    // Recurse to children
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        check_closure_captures(child, params, closure_loc);
        child = output_arena_[child].next_sibling;
    }
}

bool SemanticAnalyzer::has_spread_arg(NodeIndex call_node) const {
    if (call_node == NULL_NODE) return false;
    NodeIndex arg = output_arena_[call_node].first_child;
    while (arg != NULL_NODE) {
        const Node& a = output_arena_[arg];
        if (a.type == NodeType::Argument &&
            std::holds_alternative<Node::ArgumentData>(a.data) &&
            a.as_argument().spread_source != NULL_NODE) {
            return true;
        }
        arg = output_arena_[arg].next_sibling;
    }
    return false;
}

bool SemanticAnalyzer::reorder_named_arguments(NodeIndex call_node,
                                                const BuiltinInfo& builtin,
                                                const std::string& func_name) {
    Node& call = output_arena_[call_node];

    // Collect all arguments
    struct ArgInfo {
        NodeIndex node;
        std::optional<std::string> name;
        int target_pos;  // Position in reordered list (-1 = unknown)
    };
    std::vector<ArgInfo> args;

    NodeIndex arg = call.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = output_arena_[arg];
        std::optional<std::string> arg_name;
        if (arg_node.type == NodeType::Argument) {
            arg_name = arg_node.as_arg_name();
        }
        args.push_back({arg, arg_name, -1});
        arg = output_arena_[arg].next_sibling;
    }

    if (args.empty()) return true;

    // Check for named arguments and determine if reordering is needed
    bool has_named = false;
    bool seen_named = false;
    std::set<std::string> used_params;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].name.has_value()) {
            has_named = true;
            seen_named = true;

            const std::string& name = *args[i].name;

            // Check for duplicate parameter
            if (used_params.count(name)) {
                error("E010", "Duplicate named argument '" + name + "' in call to '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            used_params.insert(name);

            // Find parameter index
            int param_idx = builtin.find_param(name);
            if (param_idx < 0) {
                error("E011", "Unknown parameter '" + name + "' for function '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            args[i].target_pos = param_idx;
        } else {
            // Positional argument
            if (seen_named) {
                error("E009", "Positional argument cannot follow named argument in call to '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            // Positional args fill slots in order
            args[i].target_pos = static_cast<int>(i);
        }
    }

    if (!has_named) {
        return true;  // No reordering needed
    }

    // Check that positional args don't conflict with named args
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].name.has_value()) {
            // Check if this positional slot was also filled by a named arg
            for (std::size_t j = 0; j < args.size(); ++j) {
                if (args[j].name.has_value() && args[j].target_pos == static_cast<int>(i)) {
                    error("E012", "Parameter '" + *args[j].name + "' at position " +
                          std::to_string(i) + " conflicts with positional argument in call to '" +
                          func_name + "'", output_arena_[args[i].node].location);
                    return false;
                }
            }
        }
    }

    // Reorder arguments: create array indexed by target position
    std::size_t max_pos = 0;
    for (const auto& a : args) {
        if (a.target_pos >= 0) {
            max_pos = std::max(max_pos, static_cast<std::size_t>(a.target_pos));
        }
    }

    std::vector<NodeIndex> reordered(max_pos + 1, NULL_NODE);
    for (const auto& a : args) {
        if (a.target_pos >= 0) {
            reordered[a.target_pos] = a.node;
        }
    }

    // Clear argument names after reordering (they're now positional)
    for (auto& a : args) {
        if (a.name.has_value() && a.node != NULL_NODE) {
            Node& arg_node = output_arena_[a.node];
            if (arg_node.type == NodeType::Argument) {
                arg_node.data = Node::ArgumentData{std::nullopt};
            }
        }
    }

    // Fill gaps (skipped positionals between named args) with `_` placeholder
    // identifiers so the codegen default-filling path still runs for those
    // slots. Without this, `phaser(sig, 0.5, 0.8, 200, 4000, lfo_phase: 0.3)`
    // would collapse to a 6-arg call and the user's lfo_phase value would
    // bind to feedback instead of lfo_phase.
    const SourceLocation call_loc = call.location;
    for (std::size_t i = 0; i < reordered.size(); ++i) {
        if (reordered[i] != NULL_NODE) continue;
        NodeIndex underscore = output_arena_.alloc(NodeType::Identifier, call_loc);
        output_arena_[underscore].data = Node::IdentifierData{"_"};
        output_arena_[underscore].next_sibling = NULL_NODE;
        reordered[i] = underscore;
    }

    // Rebuild child list in new order
    call.first_child = NULL_NODE;
    NodeIndex prev = NULL_NODE;
    for (NodeIndex idx : reordered) {
        if (idx == NULL_NODE) continue;

        output_arena_[idx].next_sibling = NULL_NODE;

        if (prev == NULL_NODE) {
            call.first_child = idx;
        } else {
            output_arena_[prev].next_sibling = idx;
        }
        prev = idx;
    }

    return true;
}

bool SemanticAnalyzer::reorder_named_arguments(NodeIndex call_node,
                                                const std::vector<std::string>& param_names,
                                                const std::string& func_name) {
    Node& call = output_arena_[call_node];

    // Collect all arguments
    struct ArgInfo {
        NodeIndex node;
        std::optional<std::string> name;
        int target_pos;
    };
    std::vector<ArgInfo> args;

    NodeIndex arg = call.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = output_arena_[arg];
        std::optional<std::string> arg_name;
        if (arg_node.type == NodeType::Argument) {
            arg_name = arg_node.as_arg_name();
        }
        args.push_back({arg, arg_name, -1});
        arg = output_arena_[arg].next_sibling;
    }

    if (args.empty()) return true;

    // Check for named arguments and determine if reordering is needed
    bool has_named = false;
    bool seen_named = false;
    std::set<std::string> used_params;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].name.has_value()) {
            has_named = true;
            seen_named = true;

            const std::string& name = *args[i].name;

            if (used_params.count(name)) {
                error("E010", "Duplicate named argument '" + name + "' in call to '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            used_params.insert(name);

            // Find parameter index by linear search
            int param_idx = -1;
            for (std::size_t j = 0; j < param_names.size(); ++j) {
                if (param_names[j] == name) {
                    param_idx = static_cast<int>(j);
                    break;
                }
            }
            if (param_idx < 0) {
                error("E011", "Unknown parameter '" + name + "' for function '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            args[i].target_pos = param_idx;
        } else {
            if (seen_named) {
                error("E009", "Positional argument cannot follow named argument in call to '" +
                      func_name + "'", output_arena_[args[i].node].location);
                return false;
            }
            args[i].target_pos = static_cast<int>(i);
        }
    }

    if (!has_named) {
        return true;
    }

    // Check that positional args don't conflict with named args
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!args[i].name.has_value()) {
            for (std::size_t j = 0; j < args.size(); ++j) {
                if (args[j].name.has_value() && args[j].target_pos == static_cast<int>(i)) {
                    error("E012", "Parameter '" + *args[j].name + "' at position " +
                          std::to_string(i) + " conflicts with positional argument in call to '" +
                          func_name + "'", output_arena_[args[i].node].location);
                    return false;
                }
            }
        }
    }

    // Reorder arguments: create array indexed by target position
    std::size_t max_pos = 0;
    for (const auto& a : args) {
        if (a.target_pos >= 0) {
            max_pos = std::max(max_pos, static_cast<std::size_t>(a.target_pos));
        }
    }

    std::vector<NodeIndex> reordered(max_pos + 1, NULL_NODE);
    for (const auto& a : args) {
        if (a.target_pos >= 0) {
            reordered[a.target_pos] = a.node;
        }
    }

    // Clear argument names after reordering
    for (auto& a : args) {
        if (a.name.has_value() && a.node != NULL_NODE) {
            Node& arg_node = output_arena_[a.node];
            if (arg_node.type == NodeType::Argument) {
                arg_node.data = Node::ArgumentData{std::nullopt};
            }
        }
    }

    // Rebuild child list in new order
    call.first_child = NULL_NODE;
    NodeIndex prev = NULL_NODE;
    for (NodeIndex idx : reordered) {
        if (idx == NULL_NODE) continue;

        output_arena_[idx].next_sibling = NULL_NODE;

        if (prev == NULL_NODE) {
            call.first_child = idx;
        } else {
            output_arena_[prev].next_sibling = idx;
        }
        prev = idx;
    }

    return true;
}

} // namespace akkado
