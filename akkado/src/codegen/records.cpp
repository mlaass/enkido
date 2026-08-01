// Records + argument shaping — record literals, field access, pipe bindings,
// destructuring, spread/named-arg reordering.

#include "akkado/codegen.hpp"
#include "akkado/named_args.hpp"
#include "akkado/codegen/codegen.hpp"  // Master include for all codegen helpers
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/codegen/state_init_builder.hpp"
#include "akkado/builtins.hpp"
#include "akkado/overload.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/source_map.hpp"
#include "akkado/stdlib.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/const_eval.hpp"
#include "akkado/pattern_eval.hpp"
#ifdef CEDAR_HOST_EXTENSIONS
#include "akkado/host_extensions.hpp"
#endif
#include <cedar/vm/state_pool.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace akkado {

// Use helpers from akkado::codegen namespace
using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::is_audio_rate_producer;
using codegen::is_upgradeable_oscillator;
using codegen::upgrade_for_fm;
using codegen::SamplePatternEmitCtx;
using codegen::emit_sample_chain;

// Bind destructured fields from a record/pattern TypedValue into the symbol table.
// Phase 3b: each field may carry a default expression (AST node index) used
// when the source is missing that field. Pure missing-field (no default)
// fires `missing_field_code`.
bool CodeGenerator::bind_destructure_fields(
    const TypedValue& source_tv,
    const std::vector<DestructureBinding>& fields,
    SourceLocation loc,
    const char* missing_field_code)
{
    auto bind_default_or_error = [&](const DestructureBinding& field,
                                     const char* missing_msg_for_kind) -> bool {
        if (field.default_node != NULL_NODE) {
            TypedValue def_tv = visit(field.default_node);
            if (def_tv.error) {
                return false;
            }
            symbols_->define_variable(field.name, def_tv.buffer);
            return true;
        }
        std::string msg = (std::string(missing_field_code) == "E187")
            ? "Destructure source missing required field '" + field.name + "' (no default declared)"
            : std::string("Destructure field '") + field.name + "' not found in " + missing_msg_for_kind;
        error(missing_field_code, msg, loc);
        return false;
    };

    if (source_tv.type == ValueType::Record && source_tv.record) {
        for (const auto& field : fields) {
            auto field_it = source_tv.record->fields.find(field.name);
            if (field_it == source_tv.record->fields.end()) {
                if (!bind_default_or_error(field, "record")) return false;
                continue;
            }
            symbols_->define_variable(field.name, field_it->second.buffer);
        }
        return true;
    }

    if (source_tv.type == ValueType::Pattern && source_tv.pattern) {
        for (const auto& field : fields) {
            int idx = pattern_field_index(field.name);
            if (idx < 0 || source_tv.pattern->fields[static_cast<std::size_t>(idx)] == 0xFFFF) {
                if (!bind_default_or_error(field, "pattern")) return false;
                continue;
            }
            symbols_->define_variable(field.name, source_tv.pattern->fields[static_cast<std::size_t>(idx)]);
        }
        return true;
    }

    error("E140", "Cannot destructure: scrutinee has no record fields", loc);
    return false;
}

// Array HOF implementations are in codegen_arrays.cpp

// ============================================================================
// Argument-spread expansion
// ============================================================================

std::optional<std::vector<CodeGenerator::ExpandedArg>>
CodeGenerator::expand_call_arguments(NodeIndex call_node) {
    std::vector<ExpandedArg> out;
    bool saw_record_spread = false;
    bool saw_array_spread = false;

    NodeIndex arg = ast_->arena[call_node].first_child;
    while (arg != NULL_NODE) {
        const Node& a = ast_->arena[arg];

        // Spread argument: `..expr` — Argument node with a spread extra child.
        if (a.type == NodeType::Argument &&
            a.extra_child(0) != NULL_NODE) {

            NodeIndex src_node = a.extra_child(0);
            TypedValue src_tv = visit(src_node);
            SourceLocation src_loc = ast_->arena[src_node].location;

            if (src_tv.type == ValueType::Record && src_tv.record) {
                if (saw_array_spread) {
                    error("E180", "Cannot mix record and array spread in one call",
                          src_loc);
                    return std::nullopt;
                }
                saw_record_spread = true;

                for (const auto& [name, tv] : src_tv.record->fields) {
                    ExpandedArg ea;
                    ea.name = name;
                    ea.source_node = NULL_NODE;
                    ea.resolved = tv;
                    ea.loc = src_loc;
                    out.push_back(std::move(ea));
                }
            } else if (src_tv.type == ValueType::Array && src_tv.array) {
                if (saw_record_spread) {
                    error("E180", "Cannot mix record and array spread in one call",
                          src_loc);
                    return std::nullopt;
                }
                saw_array_spread = true;

                for (const auto& el : src_tv.array->elements) {
                    ExpandedArg ea;
                    ea.name = std::nullopt;
                    ea.source_node = NULL_NODE;
                    ea.resolved = el;
                    ea.loc = src_loc;
                    out.push_back(std::move(ea));
                }
            } else if (src_tv.type == ValueType::Pattern && src_tv.pattern) {
                // Patterns expose record-like fields (freq, vel, trig, gate, type)
                if (saw_array_spread) {
                    error("E180", "Cannot mix record and array spread in one call",
                          src_loc);
                    return std::nullopt;
                }
                saw_record_spread = true;

                static const char* field_names[] = {"freq", "vel", "trig", "gate", "type"};
                for (std::size_t i = 0; i < 5; ++i) {
                    if (src_tv.pattern->fields[i] != 0xFFFF) {
                        ExpandedArg ea;
                        ea.name = field_names[i];
                        ea.source_node = NULL_NODE;
                        ea.resolved = TypedValue::signal(src_tv.pattern->fields[i]);
                        ea.loc = src_loc;
                        out.push_back(std::move(ea));
                    }
                }
            } else {
                error("E140", "Spread source is not a record or array", src_loc);
                return std::nullopt;
            }
        } else {
            // Concrete argument: positional or named (..pass through).
            ExpandedArg ea;
            if (a.type == NodeType::Argument &&
                std::holds_alternative<Node::ArgumentData>(a.data)) {
                ea.name = a.as_argument().name;
                ea.source_node = (a.first_child != NULL_NODE) ? a.first_child : arg;
            } else {
                // Bare expression (rare — record fields use RecordFieldData).
                ea.source_node = arg;
            }
            ea.loc = a.location;
            out.push_back(std::move(ea));
        }

        arg = ast_->arena[arg].next_sibling;
    }

    return out;
}

bool CodeGenerator::reorder_spread_named_args(const BuiltinInfo& builtin,
                                              const std::string& func_name,
                                              std::vector<ExpandedArg>& args,
                                              SourceLocation call_loc) {
    if (args.empty()) return true;

    std::vector<NamedArgInput> inputs;
    inputs.reserve(args.size());
    for (const auto& a : args) {
        inputs.push_back({a.name, a.loc});
    }

    // Named args map by find_param() into the unified regular+extended
    // index space. Unknown field names are non-fatal here (PRD decision):
    // warned (W160) and dropped, matching the user-function spread path.
    auto find_param = [&](std::string_view name, std::size_t) -> int {
        return builtin.find_param(name);
    };
    NamedArgSlots slots = assign_named_arg_slots(inputs, func_name,
                                                 find_param,
                                                 /*drop_unknown=*/true);

    for (std::size_t idx : slots.dropped) {
        std::string sig;
        for (std::size_t p = 0; p < MAX_BUILTIN_PARAMS &&
                 !builtin.param_names[p].empty(); ++p) {
            if (!sig.empty()) sig += ", ";
            sig += std::string(builtin.param_names[p]);
        }
        for (std::size_t p = 0; p < builtin.extended_param_count &&
                 p < MAX_EXTENDED_PARAMS &&
                 !builtin.extended_param_names[p].empty(); ++p) {
            if (!sig.empty()) sig += ", ";
            sig += std::string(builtin.extended_param_names[p]);
        }
        warn("W160", "Spread field '" + *args[idx].name +
             "' has no matching parameter in " + func_name +
             "(" + sig + ")", args[idx].loc);
    }

    if (!slots.ok) {
        if (slots.code) error(slots.code, slots.message, slots.error_loc);
        return false;
    }
    if (!slots.has_named) return true;  // already positional
    if (slots.slot_source.empty()) {
        // Every argument was a dropped unknown field — emit an empty call.
        args.clear();
        return true;
    }

    // Materialise the slot-ordered ExpandedArg vector. Names are positional
    // after reorder — clear them so downstream logic ignores them. Underscore
    // gap-fills carry call_loc and is_underscore=true; the per-arg loop turns
    // each into the parameter's declared default (or E106).
    std::vector<ExpandedArg> reordered;
    reordered.reserve(slots.slot_source.size());
    for (std::size_t i = 0; i < slots.slot_source.size(); ++i) {
        if (slots.slot_source[i] < 0) {
            ExpandedArg gap;
            gap.name = std::nullopt;
            gap.source_node = NULL_NODE;
            gap.resolved = std::nullopt;
            gap.loc = call_loc;
            gap.is_underscore = true;
            reordered.push_back(std::move(gap));
        } else {
            ExpandedArg moved = std::move(
                args[static_cast<std::size_t>(slots.slot_source[i])]);
            moved.name = std::nullopt;
            reordered.push_back(std::move(moved));
        }
    }

    args = std::move(reordered);
    return true;
}

// ============================================================================
// Record support implementation
// ============================================================================

TypedValue CodeGenerator::handle_record_literal(NodeIndex node, const Node& n) {
    // Record literals expand to multiple buffers - one per field
    // We track the field->TypedValue mapping in RecordPayload

    std::unordered_map<std::string, TypedValue> field_values;
    std::uint16_t first_buffer = BufferAllocator::BUFFER_UNUSED;

    // Handle spread source: {..base, field: value} — extra_children[0]
    {
        const NodeIndex spread_src = n.extra_child(0);
        if (spread_src != NULL_NODE) {
            // Visit the spread source expression
            TypedValue spread_tv = visit(spread_src);

            // If the spread source is a record, copy its fields
            if (spread_tv.type == ValueType::Record && spread_tv.record) {
                for (const auto& [name, tv] : spread_tv.record->fields) {
                    field_values[name] = tv;
                    if (first_buffer == BufferAllocator::BUFFER_UNUSED) {
                        first_buffer = tv.buffer;
                    }
                }
            } else if (spread_tv.type == ValueType::Pattern && spread_tv.pattern) {
                // Pattern as spread source - extract known fields
                static const char* field_names[] = {"freq", "vel", "trig", "gate", "type"};
                for (int i = 0; i < 5; ++i) {
                    if (spread_tv.pattern->fields[i] != 0xFFFF) {
                        field_values[field_names[i]] = TypedValue::signal(spread_tv.pattern->fields[i]);
                        if (first_buffer == BufferAllocator::BUFFER_UNUSED) {
                            first_buffer = spread_tv.pattern->fields[i];
                        }
                    }
                }
            } else {
                // Follow symbol table for identifier spread sources
                const Node& spread_node = ast_->arena[spread_src];
                if (spread_node.type == NodeType::Identifier) {
                    std::string var_name;
                    if (std::holds_alternative<Node::IdentifierData>(spread_node.data)) {
                        var_name = ctx_->interner->view(spread_node.as_identifier());
                    }
                    auto sym = symbols_->lookup(var_name);
                    if (sym && sym->typed_value && sym->typed_value->type == ValueType::Record &&
                        sym->typed_value->record) {
                        for (const auto& [name, tv] : sym->typed_value->record->fields) {
                            field_values[name] = tv;
                            if (first_buffer == BufferAllocator::BUFFER_UNUSED) {
                                first_buffer = tv.buffer;
                            }
                        }
                    } else if (sym && sym->kind == SymbolKind::Record && sym->record_type) {
                        auto rec_it = node_types_.find(sym->record_type->source_node);
                        if (rec_it == node_types_.end()) {
                            visit(sym->record_type->source_node);
                            rec_it = node_types_.find(sym->record_type->source_node);
                        }
                        if (rec_it != node_types_.end() && rec_it->second.type == ValueType::Record &&
                            rec_it->second.record) {
                            for (const auto& [name, tv] : rec_it->second.record->fields) {
                                field_values[name] = tv;
                                if (first_buffer == BufferAllocator::BUFFER_UNUSED) {
                                    first_buffer = tv.buffer;
                                }
                            }
                        } else {
                            error("E140", "Spread source is not a record", ast_->arena[spread_src].location);
                        }
                    } else {
                        error("E140", "Spread source is not a record", ast_->arena[spread_src].location);
                    }
                } else {
                    error("E140", "Spread source is not a record", ast_->arena[spread_src].location);
                }
            }
        }
    }

    // Iterate through explicit field children (each is an Argument with RecordFieldData)
    // These override any spread fields with the same name
    NodeIndex field_node = n.first_child;
    while (field_node != NULL_NODE) {
        const Node& field = ast_->arena[field_node];

        if (field.type == NodeType::Argument &&
            std::holds_alternative<Node::RecordFieldData>(field.data)) {

            const auto& field_data = field.as_record_field();
            const std::string& field_name = field_data.name;

            // Get the field value (first child of the Argument node)
            NodeIndex value_node = field.first_child;
            if (value_node != NULL_NODE) {
                // Generate code for the field value
                TypedValue value_tv = visit(value_node);

                // Track this field's value (overrides spread if same name)
                field_values[field_name] = value_tv;

                // Record first buffer for return value
                if (first_buffer == BufferAllocator::BUFFER_UNUSED) {
                    first_buffer = value_tv.buffer;
                }
            }
        }

        field_node = ast_->arena[field_node].next_sibling;
    }

    // Build Record TypedValue
    auto tv = TypedValue::make_record(std::move(field_values), first_buffer);
    return cache_and_return(node, tv);
}

TypedValue CodeGenerator::handle_field_access(NodeIndex node, const Node& n) {
    // Field access: expr.field
    const auto& field_data = n.as_field_access();
    const std::string& field_name = field_data.field_name;

    // Get the expression being accessed (first child)
    NodeIndex expr_node = n.first_child;
    if (expr_node == NULL_NODE) {
        error("E130", "Invalid field access: no expression", n.location);
        return TypedValue::error_val();
    }

    // Check for Module-qualified access BEFORE visiting (visiting a Module would fail)
    const Node& expr = ast_->arena[expr_node];
    if (expr.type == NodeType::Identifier) {
        std::string var_name;
        if (std::holds_alternative<Node::IdentifierData>(expr.data)) {
            var_name = ctx_->interner->view(expr.as_identifier());
        }
        auto sym = symbols_->lookup(var_name);
        if (sym && sym->kind == SymbolKind::Module) {
            std::string qname = var_name + "." + field_name;
            auto qsym = symbols_->lookup(qname);
            if (!qsym) {
                error("E504", "Module '" + var_name + "' has no definition '" + field_name + "'", n.location);
                return TypedValue::error_val();
            }
            return handle_qualified_symbol_access(node, *qsym, n.location);
        }
    }

    // Visit expression to get its TypedValue
    TypedValue expr_tv = visit(expr_node);

    // Also check the symbol table for richer type info
    if (expr.type == NodeType::Identifier) {
        std::string var_name;
        if (std::holds_alternative<Node::IdentifierData>(expr.data)) {
            var_name = ctx_->interner->view(expr.as_identifier());
        }
        auto sym = symbols_->lookup(var_name);

        // Pattern variable - generate pattern code
        if (sym && sym->kind == SymbolKind::Pattern) {
            TypedValue pat_tv = handle_pattern_reference(var_name, sym->pattern.pattern_node, n.location);
            if (pat_tv.type == ValueType::Pattern) {
                TypedValue result = pattern_field(pat_tv, field_name);
                if (!result.error) {
                    return cache_and_return(node, result);
                }
            }
            std::string avail = (pat_tv.type == ValueType::Pattern && pat_tv.pattern)
                ? available_fields(*pat_tv.pattern)
                : std::string("freq, vel, trig, gate, type");
            error("E136", "Unknown field '" + field_name +
                  "' on pattern. Available: " + avail, n.location);
            return TypedValue::error_val();
        }

        // Check symbol's typed_value for richer type info
        if (sym && sym->typed_value) {
            expr_tv = *sym->typed_value;
        }

        // Record variable
        if (sym && sym->kind == SymbolKind::Record && sym->record_type) {
            auto rec_it = node_types_.find(sym->record_type->source_node);
            if (rec_it == node_types_.end()) {
                visit(sym->record_type->source_node);
                rec_it = node_types_.find(sym->record_type->source_node);
            }
            if (rec_it != node_types_.end()) {
                expr_tv = rec_it->second;
            }
        }
    }

    // Type-based dispatch
    switch (expr_tv.type) {
        case ValueType::Pattern: {
            // PRD prd-pattern-event-arrays §5.3: `e.notes` / `e.freqs` are
            // not pattern scalar fields — they surface the event's chord as
            // a DynArray. UFCS covers only method calls, so bare field
            // access is wired here directly.
            if (field_name == "notes" || field_name == "freqs") {
                return emit_pattern_values(node, expr_node, expr_tv,
                                           /*to_midi=*/field_name == "notes",
                                           n.location);
            }

            TypedValue result = pattern_field(expr_tv, field_name);
            if (!result.error) {
                return cache_and_return(node, result);
            }
            std::string avail = expr_tv.pattern
                ? available_fields(*expr_tv.pattern)
                : std::string("freq, vel, trig, gate, type");
            error("E136", "Unknown field '" + field_name +
                  "' on pattern. Available: " + avail, n.location);
            return TypedValue::error_val();
        }

        case ValueType::Record: {
            if (expr_tv.record) {
                auto field_it = expr_tv.record->fields.find(field_name);
                if (field_it != expr_tv.record->fields.end()) {
                    return cache_and_return(node, field_it->second);
                }
            }
            // Build error message with available fields
            std::string available;
            if (expr_tv.record) {
                bool first = true;
                for (const auto& [name, _] : expr_tv.record->fields) {
                    if (!first) available += ", ";
                    available += name;
                    first = false;
                }
            }
            error("E131", "Unknown field '" + field_name + "'" +
                  (available.empty() ? "" : ". Available: " + available), n.location);
            return TypedValue::error_val();
        }

        case ValueType::StateCell: {
            // Phase 4b: read sugar — `cell.field` desugars to `get(cell).field`
            // when the cell holds a record. Each access emits a single
            // STATE_OP rate=1 against the per-field sub-cell, observably
            // identical to the field-pick from a freshly fanned-out get().
            // Scalar cells reach this branch with `expr_tv.record == nullptr`
            // and are rejected — those callers must use `get(cell)` explicitly.
            if (!expr_tv.record) {
                error("E135",
                      "Cannot access field '" + field_name +
                      "' on scalar state cell. Use get(cell) to read it.",
                      n.location);
                return TypedValue::error_val();
            }
            auto sub_it = expr_tv.record->fields.find(field_name);
            if (sub_it == expr_tv.record->fields.end()) {
                std::string available;
                bool first = true;
                for (const auto& [name, _] : expr_tv.record->fields) {
                    if (!first) available += ", ";
                    available += name;
                    first = false;
                }
                error("E136",
                      "Unknown field '" + field_name +
                      "' on state cell" +
                      (available.empty() ? "" : ". Available: " + available),
                      n.location);
                return TypedValue::error_val();
            }
            const TypedValue& sub_cell = sub_it->second;

            const std::uint16_t out =
                codegen::InstructionBuilder(cedar::Opcode::STATE_OP)
                    .rate(1)  // load mode
                    .state_id(sub_cell.cell_state_id)
                    .emit(*this, n.location);
            if (out == BufferAllocator::BUFFER_UNUSED) {
                return TypedValue::error_val();
            }

            return cache_and_return(node, TypedValue::signal(out));
        }

        case ValueType::Signal:
        case ValueType::Number:
            error("E135", "Cannot access field '" + field_name + "' on " +
                  std::string(value_type_name(expr_tv.type)) +
                  " value. Field access requires a Pattern or Record", n.location);
            return TypedValue::error_val();

        case ValueType::Array:
            error("E135", "Cannot access field '" + field_name + "' on Array. "
                  "Use indexing or array functions instead", n.location);
            return TypedValue::error_val();

        case ValueType::Function:
        case ValueType::String:
        case ValueType::DynArray:
        case ValueType::Void:
            error("E135", "Cannot access field '" + field_name + "' on " +
                  std::string(value_type_name(expr_tv.type)) + " value", n.location);
            return TypedValue::error_val();
    }

    error("E135", "Field access on expression type not supported", n.location);
    return TypedValue::error_val();
}

TypedValue CodeGenerator::handle_qualified_symbol_access(NodeIndex node, const Symbol& qsym, SourceLocation loc) {
    // Dispatch based on symbol kind — mirrors the Identifier case in visit()
    switch (qsym.kind) {
        case SymbolKind::Variable:
        case SymbolKind::Parameter: {
            std::uint16_t buf = qsym.buffer_index;
            TypedValue tv = TypedValue::signal(buf);
            if (!qsym.multi_buffers.empty()) {
                std::vector<TypedValue> elements;
                for (auto b : qsym.multi_buffers) {
                    elements.push_back(TypedValue::signal(b));
                }
                tv = TypedValue::make_array(std::move(elements), buf);
            }
            if (qsym.typed_value) {
                tv = *qsym.typed_value;
            }
            return cache_and_return(node, tv);
        }
        case SymbolKind::Pattern:
            return handle_pattern_reference(qsym.name, qsym.pattern.pattern_node, loc);
        case SymbolKind::Array: {
            if (qsym.array.source_node == NULL_NODE) {
                if (!qsym.array.buffer_indices.empty()) {
                    std::vector<TypedValue> elements;
                    for (auto b : qsym.array.buffer_indices) {
                        elements.push_back(TypedValue::signal(b));
                    }
                    auto tv = TypedValue::make_array(std::move(elements), qsym.array.buffer_indices[0]);
                    return cache_and_return(node, tv);
                }
                return cache_and_return(node, TypedValue::signal(buffers_.allocate()));
            }
            TypedValue source_tv = visit(qsym.array.source_node);
            return cache_and_return(node, source_tv);
        }
        case SymbolKind::Record:
            if (qsym.record_type) {
                auto type_it = node_types_.find(qsym.record_type->source_node);
                if (type_it != node_types_.end() && type_it->second.type == ValueType::Record) {
                    return cache_and_return(node, type_it->second);
                }
                TypedValue source_tv = visit(qsym.record_type->source_node);
                return cache_and_return(node, source_tv);
            }
            break;
        case SymbolKind::UserFunction:
        case SymbolKind::FunctionValue:
            return TypedValue::function_val();
        default:
            break;
    }
    error("E504", "Cannot access module member '" + qsym.name + "'", loc);
    return TypedValue::error_val();
}

TypedValue CodeGenerator::handle_pipe_binding(NodeIndex node, const Node& n) {
    // Pipe binding: expr as name
    const auto& binding_data = n.as_pipe_binding();
    const std::string& binding_name = binding_data.binding_name;

    // Get the bound expression (first child)
    NodeIndex expr_node = n.first_child;
    if (expr_node == NULL_NODE) {
        error("E140", "Invalid pipe binding: no expression", n.location);
        return TypedValue::error_val();
    }

    // Generate code for the expression
    TypedValue expr_tv = visit(expr_node);

    // Bind with full type info in symbol table
    if (expr_tv.type == ValueType::Record && expr_tv.record) {
        // Record binding
        auto record_type = std::make_shared<RecordTypeInfo>();
        record_type->source_node = expr_node;
        for (const auto& [name, tv] : expr_tv.record->fields) {
            RecordFieldInfo field_info;
            field_info.name = name;
            field_info.buffer_index = tv.buffer;
            field_info.field_kind = SymbolKind::Variable;
            record_type->fields.push_back(std::move(field_info));
        }
        symbols_->define_record(binding_name, record_type);

        // Also store typed_value for richer access
        auto sym = symbols_->lookup(binding_name);
        if (sym) {
            Symbol updated = *sym;
            updated.typed_value = expr_tv;
            symbols_->define(updated);
        }
    } else if (expr_tv.type == ValueType::Pattern) {
        // Pattern binding - store as variable with typed_value
        Symbol new_sym;
        new_sym.kind = SymbolKind::Variable;
        new_sym.name = binding_name;
        new_sym.name_id = ctx_->interner->intern(binding_name);
        new_sym.buffer_index = expr_tv.buffer;
        new_sym.typed_value = expr_tv;
        symbols_->define(new_sym);
    } else if (expr_tv.type == ValueType::Array && expr_tv.array) {
        Symbol new_sym;
        new_sym.kind = SymbolKind::Variable;
        new_sym.name = binding_name;
        new_sym.name_id = ctx_->interner->intern(binding_name);
        new_sym.buffer_index = expr_tv.buffer;
        new_sym.multi_buffers = buffers_of(expr_tv);
        new_sym.typed_value = expr_tv;
        symbols_->define(new_sym);
    } else {
        // Also check if identifier propagates record type from symbol
        const Node& expr = ast_->arena[expr_node];
        if (expr.type == NodeType::Identifier) {
            std::string var_name;
            if (std::holds_alternative<Node::IdentifierData>(expr.data)) {
                var_name = ctx_->interner->view(expr.as_identifier());
            }
            auto sym = symbols_->lookup(var_name);
            if (sym && sym->kind == SymbolKind::Record && sym->record_type) {
                symbols_->define_record(binding_name, sym->record_type);
                // Propagate typed_value
                auto new_sym_opt = symbols_->lookup(binding_name);
                if (new_sym_opt) {
                    Symbol updated = *new_sym_opt;
                    // Get the TypedValue for the source record
                    auto rec_it = node_types_.find(sym->record_type->source_node);
                    if (rec_it != node_types_.end()) {
                        updated.typed_value = rec_it->second;
                    }
                    symbols_->define(updated);
                }
            } else {
                symbols_->define_variable(binding_name, expr_tv.buffer);
            }
        } else {
            symbols_->define_variable(binding_name, expr_tv.buffer);
        }
    }

    // Return the expression typed value
    return cache_and_return(node, expr_tv);
}

} // namespace akkado
