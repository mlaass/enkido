// visit() dispatch — outer NodeType switch + literal handlers.
// The Call branch splits into per-kind emitters in Phase 4 (call_dispatch.cpp).

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

TypedValue CodeGenerator::visit(NodeIndex node) {
    if (node == NULL_NODE) return TypedValue::void_val();

    // Check if already visited
    auto it = node_types_.find(node);
    if (it != node_types_.end()) {
        return it->second;
    }

    const Node& n = ast_->arena[node];

    // Track source location for any instructions emitted while processing this node
    current_source_loc_ = n.location;

    switch (n.type) {
        case NodeType::Program: {
            // Visit all statements, pushing module context for imported definitions
            NodeIndex child = n.first_child;
            TypedValue last = TypedValue::void_val();
            while (child != NULL_NODE) {
                const Node& child_node = ast_->arena[child];

                // For imported module definitions, push their module path onto
                // path_stack_ so state_ids are scoped by module origin.
                // Skip <stdlib> region to preserve backward-compatible IDs.
                bool pushed_module = false;
                if (source_map_) {
                    auto* region = source_map_->find_region(child_node.location.offset);
                    if (region &&
                        region->filename != filename_ &&
                        region->filename != STDLIB_FILENAME) {
                        push_path(region->filename);
                        pushed_module = true;
                    }
                }

                last = visit(child);

                if (pushed_module) {
                    pop_path();
                }

                child = ast_->arena[child].next_sibling;
            }
            return last;
        }

        case NodeType::StringLit: {
            // String literals are compile-time only (used for match patterns, osc type, etc.)
            // They don't have a runtime representation - return string TypedValue.
            auto tv = TypedValue::string_val(0);
            return cache_and_return(node, tv);
        }

        case NodeType::NumberLit: {
            const std::uint16_t out =
                codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                    .const_value(static_cast<float>(n.as_number()))
                    .emit(*this, n.location);
            if (out == BufferAllocator::BUFFER_UNUSED) {
                return TypedValue::error_val();
            }
            return cache_and_return(node, TypedValue::number(out));
        }

        case NodeType::BoolLit: {
            const std::uint16_t out =
                codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                    .const_value(n.as_bool() ? 1.0f : 0.0f)
                    .emit(*this, n.location);
            if (out == BufferAllocator::BUFFER_UNUSED) {
                return TypedValue::error_val();
            }
            return cache_and_return(node, TypedValue::number(out));
        }

        case NodeType::PitchLit: {
            // Emit PUSH_CONST for MIDI note, then MTOF to convert to frequency
            float midi_value = static_cast<float>(n.as_pitch());
            std::uint16_t freq_buf = emit_midi_to_freq(midi_value);
            if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }
            return cache_and_return(node, TypedValue::signal(freq_buf));
        }

        case NodeType::ArrayLit: {
            // Arrays: emit all elements as multi-buffer for polyphony support
            NodeIndex first_elem = n.first_child;
            if (first_elem == NULL_NODE) {
                // Empty array - emit a zero placeholder buffer (so .buffer is valid
                // for any consumer that reads it directly), but tag the TypedValue
                // as a zero-length Array so array operators can detect it and
                // fire their empty-array branches (e.g. mean → 0, reduce → init).
                const std::uint16_t out =
                    codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                        .const_value(0.0f)
                        .emit(*this, n.location);
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    return TypedValue::error_val();
                }
                return cache_and_return(node, TypedValue::make_array({}, out));
            }

            // Visit all elements, flattening any ..array spread elements.
            std::vector<TypedValue> elements;
            bool had_spread = false;
            NodeIndex elem = first_elem;
            while (elem != NULL_NODE) {
                const Node& en = ast_->arena[elem];
                if (en.type == NodeType::Argument &&
                    en.extra_child(0) != NULL_NODE) {
                    had_spread = true;
                    NodeIndex src_node = en.extra_child(0);
                    TypedValue src_tv = visit(src_node);
                    if (src_tv.type != ValueType::Array || !src_tv.array) {
                        error("E140", "Spread source is not an array",
                              ast_->arena[src_node].location);
                        return TypedValue::error_val();
                    }
                    for (const auto& el : src_tv.array->elements) {
                        elements.push_back(el);
                    }
                } else {
                    TypedValue elem_tv = visit(elem);
                    elements.push_back(elem_tv);
                }
                elem = ast_->arena[elem].next_sibling;
            }

            if (elements.empty()) {
                // All spreads were empty — emit a zero placeholder buffer like the
                // empty-array branch above.
                const std::uint16_t out =
                    codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                        .const_value(0.0f)
                        .emit(*this, n.location);
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    return TypedValue::error_val();
                }
                return cache_and_return(node, TypedValue::make_array({}, out));
            }

            // Single non-spread element: return its value directly so the
            // call site stays a Signal/Number/etc. With a spread we always
            // wrap, even at length 1, since the user wrote `[..a, ...]`.
            if (elements.size() == 1 && !had_spread) {
                return cache_and_return(node, elements[0]);
            }

            // Multi-element array
            std::uint16_t first_buf = elements[0].buffer;
            auto tv = TypedValue::make_array(std::move(elements), first_buf);
            return cache_and_return(node, tv);
        }

        case NodeType::Index: {
            // Array indexing: arr[i]
            NodeIndex arr_node = n.first_child;
            if (arr_node == NULL_NODE) {
                error("E111", "Invalid index expression: no array", n.location);
                return TypedValue::error_val();
            }

            // Get the index node (second child via next_sibling)
            NodeIndex idx_node = ast_->arena[arr_node].next_sibling;
            if (idx_node == NULL_NODE) {
                error("E111", "Invalid index expression: no index", n.location);
                return TypedValue::error_val();
            }

            // Visit array first to populate type info
            TypedValue arr_tv = visit(arr_node);

            // Check if we have a compile-time known array
            std::uint8_t arr_len = 0;
            if (arr_tv.type == ValueType::Array && arr_tv.array) {
                arr_len = static_cast<std::uint8_t>(arr_tv.array->elements.size());
            }

            // DynArray (PRD prd-pattern-event-arrays §5.5): data is already
            // packed in one buffer; the length is a runtime signal. Emit
            // ARRAY_INDEX directly with the runtime len_buffer — wrap mode
            // handles per-sample varying length and the empty-event case.
            if (arr_tv.type == ValueType::DynArray && arr_tv.dyn) {
                TypedValue idx_tv = visit(idx_node);
                const std::uint16_t out =
                    codegen::InstructionBuilder(cedar::Opcode::ARRAY_INDEX)
                        .inputs({arr_tv.dyn->data_buffer, idx_tv.buffer,
                                 arr_tv.dyn->len_buffer})
                        .rate(0)  // 0 = wrap mode
                        .emit(*this, n.location);
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    return TypedValue::error_val();
                }
                return cache_and_return(node, TypedValue::signal(out));
            }

            // Check if index is a constant number
            const Node& idx = ast_->arena[idx_node];
            if (idx.type == NodeType::NumberLit && arr_len > 0) {
                // Constant index - direct array element access
                int idx_val = static_cast<int>(idx.as_number());

                // Handle negative indices (wrap)
                if (idx_val < 0) {
                    idx_val = ((idx_val % arr_len) + arr_len) % arr_len;
                } else if (idx_val >= arr_len) {
                    idx_val = idx_val % arr_len;
                }

                // For compile-time unrolled arrays, return the specific element
                if (arr_tv.type == ValueType::Array && arr_tv.array) {
                    TypedValue result = arr_tv.array->elements[static_cast<std::size_t>(idx_val)];
                    return cache_and_return(node, result);
                }

                // For runtime arrays, emit ARRAY_UNPACK
                const std::uint16_t out =
                    codegen::InstructionBuilder(cedar::Opcode::ARRAY_UNPACK)
                        .input(0, arr_tv.buffer)
                        .rate(static_cast<std::uint8_t>(idx_val))
                        .emit(*this, n.location);
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    return TypedValue::error_val();
                }

                return cache_and_return(node, TypedValue::signal(out));
            }

            // Dynamic index - need to emit ARRAY_INDEX for per-sample indexing
            // This requires the array to be packed into a single buffer
            std::uint16_t arr_buf = arr_tv.buffer;

            // If we have an array TypedValue, we need to pack it first using ARRAY_PACK
            if (arr_tv.type == ValueType::Array && arr_tv.array) {
                auto buffers = buffers_of(arr_tv);
                arr_len = static_cast<std::uint8_t>(buffers.size());

                // Pack multi-buffer into single array buffer
                // For arrays larger than 5, we need multiple ARRAY_PACK calls
                // Pack first 5 elements
                std::uint8_t pack_count = std::min(arr_len, static_cast<std::uint8_t>(5));
                codegen::InstructionBuilder pack_builder(cedar::Opcode::ARRAY_PACK);
                pack_builder.rate(pack_count);
                for (std::uint8_t i = 0; i < pack_count; ++i) {
                    pack_builder.input(i, buffers[i]);
                }
                std::uint16_t packed_buf = pack_builder.emit(*this, n.location);
                if (packed_buf == BufferAllocator::BUFFER_UNUSED) {
                    return TypedValue::error_val();
                }

                // For arrays > 5 elements, we need to pack remaining with ARRAY_PUSH
                for (std::uint8_t i = 5; i < arr_len; ++i) {
                    const std::uint16_t new_packed =
                        codegen::InstructionBuilder(cedar::Opcode::ARRAY_PUSH)
                            .inputs({packed_buf, buffers[i]})
                            .rate(i)  // Current length before push
                            .emit(*this, n.location);
                    if (new_packed == BufferAllocator::BUFFER_UNUSED) {
                        return TypedValue::error_val();
                    }

                    packed_buf = new_packed;
                }

                arr_buf = packed_buf;
            }

            // Now emit ARRAY_INDEX for dynamic per-sample indexing
            TypedValue idx_tv = visit(idx_node);
            std::uint16_t out = alloc_buffer(n.location);
            if (out == BufferAllocator::BUFFER_UNUSED) {
                return TypedValue::error_val();
            }

            // Create a constant buffer with the array length for ARRAY_INDEX
            const std::uint16_t len_buf =
                codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                    .const_value(static_cast<float>(arr_len > 0 ? arr_len : 1))
                    .emit(*this, n.location);
            if (len_buf == BufferAllocator::BUFFER_UNUSED) {
                return TypedValue::error_val();
            }

            codegen::InstructionBuilder(cedar::Opcode::ARRAY_INDEX)
                .inputs({arr_buf, idx_tv.buffer, len_buf})
                .rate(0)  // 0 = wrap mode (default), 1 = clamp mode
                .output(out)
                .emit(*this);

            return cache_and_return(node, TypedValue::signal(out));
        }

        case NodeType::Identifier: {
            std::string name = std::string(ctx_->interner->view(n.as_identifier()));

            // Builtin variable read (bpm, sr) — desugar to ENV_GET
            {
                const BuiltinVarDef* bv_def = lookup_builtin_variable(name);
                if (bv_def != nullptr) {
                    const auto& bv = *bv_def;
                    std::uint32_t key_hash = cedar::fnv1a_hash_runtime(
                        bv.env_key.data(), bv.env_key.size());

                    // Emit PUSH_CONST for default/fallback value
                    const std::uint16_t fallback_buf =
                        codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                            .const_value(bv.default_value)
                            .emit(*this, n.location);
                    if (fallback_buf == BufferAllocator::BUFFER_UNUSED) {
                        return TypedValue::error_val();
                    }

                    // Emit ENV_GET with reserved key hash
                    const std::uint16_t out_buf =
                        codegen::InstructionBuilder(cedar::Opcode::ENV_GET)
                            .input(0, fallback_buf)
                            .state_id(key_hash)
                            .emit(*this, n.location);
                    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
                        return TypedValue::error_val();
                    }

                    return cache_and_return(node, TypedValue::signal(out_buf));
                }
            }

            auto sym = symbols_->lookup(name);

            if (!sym) {
                error("E102", "Undefined identifier: '" + name + "'", n.location);
                return TypedValue::error_val();
            }

            if (sym->kind == SymbolKind::Variable || sym->kind == SymbolKind::Parameter) {
                // Return the buffer index from the symbol table
                std::uint16_t buf = sym->buffer_index;
                TypedValue tv = TypedValue::signal(buf);

                // Propagate multi-buffer info from symbol to this identifier node
                if (!sym->multi_buffers.empty()) {
                    std::vector<TypedValue> elements;
                    for (auto b : sym->multi_buffers) {
                        elements.push_back(TypedValue::signal(b));
                    }
                    tv = TypedValue::make_array(std::move(elements), buf);
                }

                // Check if symbol has a typed_value (from pipe binding)
                if (sym->typed_value) {
                    tv = *sym->typed_value;
                }

                // Promote to Stereo when the symbol's buffer is the left side
                // of a known stereo pair. This lets later passes (out()
                // validation, auto-lift) see the channel type through a
                // `let`-bound variable like `s = stereo(...)`.
                if (tv.type == ValueType::Signal && tv.channels == ChannelCount::Mono) {
                    auto pair_it = stereo_buffer_pairs_.find(tv.buffer);
                    if (pair_it != stereo_buffer_pairs_.end()) {
                        tv = TypedValue::stereo_signal(tv.buffer, pair_it->second);
                    }
                }

                return cache_and_return(node, tv);
            }

            if (sym->kind == SymbolKind::Pattern) {
                // Pattern variable - generate code for this pattern
                return handle_pattern_reference(name, sym->pattern.pattern_node, n.location);
            }

            if (sym->kind == SymbolKind::Array) {
                if (sym->array.source_node == NULL_NODE) {
                    // Synthetic array (rest param) — buffers already computed
                    if (!sym->array.buffer_indices.empty()) {
                        std::vector<TypedValue> elements;
                        for (auto b : sym->array.buffer_indices) {
                            elements.push_back(TypedValue::signal(b));
                        }
                        auto tv = TypedValue::make_array(std::move(elements),
                                                          sym->array.buffer_indices[0]);
                        return cache_and_return(node, tv);
                    }
                    // Empty rest → zero. (Historically no E101 check here —
                    // keep manual allocation to preserve that behaviour.)
                    auto zero_buf = buffers_.allocate();
                    codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                        .const_value(0.0f)
                        .output(zero_buf)
                        .emit(*this);
                    return cache_and_return(node, TypedValue::signal(zero_buf));
                }

                // Array variable - visit the source node to generate array code
                TypedValue source_tv = visit(sym->array.source_node);
                return cache_and_return(node, source_tv);
            }

            if (sym->kind == SymbolKind::Record && sym->record_type) {
                // Record variable - check if we've already generated code for it
                auto type_it = node_types_.find(sym->record_type->source_node);
                if (type_it != node_types_.end() && type_it->second.type == ValueType::Record) {
                    return cache_and_return(node, type_it->second);
                }

                // Not yet generated - visit the source node
                TypedValue source_tv = visit(sym->record_type->source_node);
                return cache_and_return(node, source_tv);
            }

            if (sym->kind == SymbolKind::FunctionValue || sym->kind == SymbolKind::UserFunction) {
                // Function values are handled specially in map() and other HOFs
                return TypedValue::function_val();
            }

            // Builtins without args? Shouldn't happen for identifiers
            error("E103", "Cannot use builtin as value: '" + name + "'", n.location);
            return TypedValue::error_val();
        }

        case NodeType::Assignment: {
            // Variable name is stored in the node's data
            // First child is the value expression
            NodeIndex value_idx = n.first_child;

            if (value_idx == NULL_NODE) {
                error("E104", "Invalid assignment", n.location);
                return TypedValue::error_val();
            }

            std::string var_name = std::string(ctx_->interner->view(n.as_identifier()));

            // Builtin variable assignment (bpm = 120) — extract compile-time constant
            {
                const BuiltinVarDef* bv_def = lookup_builtin_variable(var_name);
                if (bv_def != nullptr) {
                    const auto& bv = *bv_def;
                    if (bv.setter_name.empty()) {
                        error("E170", "Cannot assign to read-only builtin variable '" +
                              std::string(var_name) + "'", n.location);
                        return TypedValue::error_val();
                    }
                    // Evaluate RHS as compile-time constant
                    ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
                    auto const_val = evaluator.evaluate(value_idx);
                    for (const auto& diag : evaluator.diagnostics()) {
                        diagnostics_.push_back(diag);
                    }
                    if (const_val) {
                        if (auto* scalar = std::get_if<double>(&*const_val)) {
                            float fval = static_cast<float>(*scalar);
                            if (bv.min_value != 0.0f || bv.max_value != 0.0f) {
                                fval = std::max(bv.min_value, std::min(bv.max_value, fval));
                            }
                            builtin_var_overrides_.push_back({
                                std::string(var_name), fval, n.location});
                            return cache_and_return(node, TypedValue::void_val());
                        }
                        error("E171", "Builtin variable '" + std::string(var_name) +
                              "' requires a scalar value", n.location);
                        return TypedValue::error_val();
                    }
                    error("E172", "'" + std::string(var_name) +
                          "' must be a compile-time constant (e.g., bpm = 120)", n.location);
                    return TypedValue::error_val();
                }
            }

            // Check if this is a pattern assignment
            auto sym = symbols_->lookup(var_name);
            if (sym && sym->kind == SymbolKind::Pattern) {
                // Pattern assignments don't emit code here - the pattern is
                // evaluated when the variable is referenced
                return cache_and_return(node, TypedValue::void_val());
            }

            // Push variable name onto path for semantic IDs
            push_path(var_name);

            // Generate code for the value expression
            TypedValue value_tv = visit(value_idx);

            pop_path();

            // Check if visit produced a pending function ref (closure-returning function call)
            if (pending_function_ref_) {
                symbols_->define_function_value(var_name, *pending_function_ref_);
                pending_function_ref_ = std::nullopt;
                return cache_and_return(node, TypedValue::function_val());
            }

            // Update symbol table with the buffer index and multi-buffer info
            if (sym && (sym->kind == SymbolKind::Variable || sym->kind == SymbolKind::Parameter)) {
                if (value_tv.type == ValueType::Array && value_tv.array) {
                    Symbol new_sym;
                    new_sym.kind = SymbolKind::Variable;
                    new_sym.name = var_name;
                    new_sym.name_id = ctx_->interner->intern(var_name);
                    new_sym.buffer_index = value_tv.buffer;
                    new_sym.multi_buffers = buffers_of(value_tv);
                    new_sym.typed_value = value_tv;
                    symbols_->define(new_sym);
                } else if (value_tv.type == ValueType::Record ||
                           value_tv.type == ValueType::Pattern ||
                           value_tv.type == ValueType::StateCell ||
                           value_tv.type == ValueType::DynArray) {
                    // Preserve rich type info through symbol table.
                    // StateCell carries cell_state_id which get/set need to
                    // route the STATE_OP instruction to the right slot.
                    // DynArray carries the data/len buffer pair so len() and
                    // indexing keep working after `x = notes(e)`.
                    Symbol new_sym;
                    new_sym.kind = SymbolKind::Variable;
                    new_sym.name = var_name;
                    new_sym.name_id = ctx_->interner->intern(var_name);
                    new_sym.buffer_index = value_tv.buffer;
                    new_sym.typed_value = value_tv;
                    symbols_->define(new_sym);
                } else {
                    symbols_->define_variable(var_name, value_tv.buffer);
                }
            }

            return cache_and_return(node, value_tv);
        }

        case NodeType::DestructureAssignment: {
            // Statement-level destructure: {x, y} = expr
            // Evaluate RHS, then bind each named field as an immutable
            // variable using bind_destructure_fields(...). Emits E187 on
            // missing required field and E140 on non-Record/non-Pattern source.
            NodeIndex value_idx = n.first_child;
            if (value_idx == NULL_NODE) {
                error("E104", "Invalid destructure assignment", n.location);
                return TypedValue::error_val();
            }
            TypedValue value_tv = visit(value_idx);
            if (value_tv.error) {
                return cache_and_return(node, TypedValue::error_val());
            }
            bind_destructure_fields(value_tv, destructure_bindings(n), n.location, "E187");
            return cache_and_return(node, TypedValue::void_val());
        }

        case NodeType::ConstDecl: {
            // Const variable: evaluate RHS at compile time
            std::string var_name = std::string(ctx_->interner->view(n.as_identifier()));
            NodeIndex value_idx = n.first_child;

            if (value_idx == NULL_NODE) {
                error("E104", "Invalid const declaration", n.location);
                return TypedValue::error_val();
            }

            // Evaluate at compile time using ConstEvaluator
            ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
            auto const_val = evaluator.evaluate(value_idx);

            // Forward any diagnostics from const evaluator
            for (const auto& diag : evaluator.diagnostics()) {
                diagnostics_.push_back(diag);
            }

            if (!const_val) {
                error("E203", "Failed to evaluate const expression for '" + var_name + "'",
                      n.location);
                return TypedValue::error_val();
            }

            // Store const value in symbol table
            symbols_->define_const_variable(var_name, *const_val);

            // Emit PUSH_CONST instruction(s) for runtime access
            if (std::holds_alternative<double>(*const_val)) {
                float val = static_cast<float>(std::get<double>(*const_val));
                std::uint16_t buf = emit_push_const(val);
                if (buf == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    return TypedValue::error_val();
                }
                // Update symbol with buffer index
                symbols_->define_variable(var_name, buf);
                // Re-mark as const with value
                auto sym2 = symbols_->lookup(var_name);
                if (sym2) {
                    Symbol updated = *sym2;
                    updated.is_const = true;
                    updated.const_value = *const_val;
                    symbols_->define(updated);
                }
                return cache_and_return(node, TypedValue::number(buf));
            } else {
                // Array const value
                const auto& arr = std::get<std::vector<double>>(*const_val);
                std::vector<TypedValue> result_elements;
                for (double v : arr) {
                    std::uint16_t buf = emit_push_const(static_cast<float>(v));
                    if (buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        return TypedValue::error_val();
                    }
                    result_elements.push_back(TypedValue::number(buf));
                }

                // Register as array and update symbol
                std::uint16_t first_buf = result_elements.empty() ?
                    BufferAllocator::BUFFER_UNUSED : result_elements[0].buffer;
                if (!result_elements.empty()) {
                    std::vector<std::uint16_t> result_buffers;
                    for (const auto& e : result_elements) result_buffers.push_back(e.buffer);

                    auto tv = TypedValue::make_array(std::move(result_elements), first_buf);

                    Symbol sym{};
                    sym.kind = SymbolKind::Variable;
                    sym.name = var_name;
                    sym.name_id = ctx_->interner->intern(var_name);
                    sym.buffer_index = first_buf;
                    sym.multi_buffers = std::move(result_buffers);
                    sym.is_const = true;
                    sym.const_value = *const_val;
                    sym.typed_value = tv;
                    symbols_->define(sym);
                    return cache_and_return(node, tv);
                }
                return cache_and_return(node, TypedValue::void_val());
            }
        }

        case NodeType::MethodCall:
            // UFCS desugar: x.foo(a, b) ≡ foo(x, a, b). The parser already
            // shapes MethodCall identically to Call (method name in
            // IdentifierData, receiver as the first child, args following),
            // so falling through executes the call body unchanged. The
            // "Method 'X' not found" error is produced naturally by the same
            // E107 path that catches typos in regular calls.
            [[fallthrough]];
        case NodeType::Call: {
            // Function name is stored in the node's data, not as a child
            std::string func_name = std::string(ctx_->interner->view(n.as_identifier()));

            // Save the call's source location - visiting arguments may overwrite it
            SourceLocation call_loc = current_source_loc_;

            // Check user-defined functions FIRST (allows stdlib osc to work)
            auto sym = symbols_->lookup(func_name);
            if (sym && sym->kind == SymbolKind::UserFunction) {
                // Phase 4: one definition → the unchanged direct path; multiple
                // overloads → select one by argument type (warn + fall back to
                // the first overload when selection is impossible).
                if (sym->overloads.size() == 1) {
                    return handle_user_function_call(node, n, sym->overloads.front());
                }
                return dispatch_overloaded_function_call(node, n, sym->overloads);
            }

            // Check for FunctionValue (lambda assigned to variable)
            if (sym && sym->kind == SymbolKind::FunctionValue) {
                return handle_function_value_call(node, n, sym->function_ref);
            }

            // Builtin/special-handler spread expansion. PRD-parser-codegen-
            // correctness Phase 1a: the original AST is left untouched.
            // collect_spread_args fills a flat ExpandedArg list which
            // emit_builtin_call (after optional reorder for named spread
            // fields) materialises into CallSlots for its per-arg loop.
            std::vector<ExpandedArg> expanded_args;
            bool did_spread_swap = false;
            if (!collect_spread_args(node, expanded_args, did_spread_swap)) {
                return TypedValue::error_val();
            }

            // Phase-2 operator dispatch (PRD prd-builtin-overload-resolution
            // §5.4): arithmetic (+,-,*,/,^) → the array/stereo broadcasting
            // handler; comparison/logical fall through to the generic builtin
            // emission. Placed after the user-fn / FunctionValue checks so a
            // user `fn add` still shadows the operator.
            if (auto tv = try_operator_dispatch(node, n, func_name)) {
                return *tv;
            }

            // Phase-3 builtin-overload dispatch (PRD prd-builtin-overload-
            // resolution §8): the migrated multi-form families resolve through
            // lookup_builtin_overloads — LegacyHandler routing plus the
            // sample/sample_loop literal-id gate (E424). Placed after the
            // operator / user-fn checks (same shadowing rules) and before the
            // codegen_handler dispatch and generic path.
            if (auto tv = try_builtin_overload_dispatch(node, n, func_name)) {
                return *tv;
            }

            // PRD prd-codegen-sprawl-cleanup Phase 4: per-builtin custom
            // codegen dispatch lives on BuiltinInfo::codegen_handler (see
            // BuiltinHandlers in builtins.hpp). Key-set parity with the old
            // name-keyed dispatch map: only exact BUILTIN_FUNCTIONS names
            // dispatch here — aliases resolve later via the normal builtin
            // lookup path, exactly as before.
            auto handler_it = BUILTIN_FUNCTIONS.find(std::string_view(func_name));
            if (handler_it != BUILTIN_FUNCTIONS.end() &&
                handler_it->second.codegen_handler != nullptr) {
                TypedValue tv = (this->*(handler_it->second.codegen_handler))(node, n);
                // Most handlers cache via cache_and_return on success, but
                // several pattern-transform handlers (handle_fast_call et al.)
                // leave error paths uncached. Without this defensive write a
                // re-visit (e.g. the E186 channel-type check below)
                // cache-misses, re-runs the handler, and duplicates its
                // diagnostics.
                if (node_types_.find(node) == node_types_.end()) {
                    node_types_[node] = tv;
                }
                return tv;
            }

            // Special handling for mtof() - propagate multi-buffers
            if (func_name == "mtof") {
                NodeIndex arg = n.first_child;
                if (arg == NULL_NODE) {
                    error("E135", "mtof() requires 1 argument", n.location);
                    return TypedValue::error_val();
                }

                const Node& arg_node = ast_->arena[arg];
                NodeIndex midi_node = (arg_node.type == NodeType::Argument) ?
                                      arg_node.first_child : arg;

                // Visit to populate type info
                TypedValue midi_tv = visit(midi_node);

                // Check if input is multi-buffer (e.g., from chord())
                if (midi_tv.type == ValueType::Array && midi_tv.array) {
                    auto midi_buffers = buffers_of(midi_tv);
                    std::vector<TypedValue> freq_elements;
                    freq_elements.reserve(midi_buffers.size());

                    // Restore call location for emitting mtof instructions
                    current_source_loc_ = call_loc;

                    for (std::uint16_t mb : midi_buffers) {
                        const std::uint16_t freq_buf =
                            codegen::InstructionBuilder(cedar::Opcode::MTOF)
                                .input(0, mb)
                                .input(4, 0)  // preserved zero-init (was never set to 0xFFFF)
                                .emit(*this, n.location);
                        if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                            return TypedValue::error_val();
                        }

                        freq_elements.push_back(TypedValue::signal(freq_buf));
                    }

                    std::uint16_t first_buf = freq_elements[0].buffer;
                    auto tv = TypedValue::make_array(std::move(freq_elements), first_buf);
                    return cache_and_return(node, tv);
                }

                // Single buffer case - fall through to normal handling
            }

            // Generic builtin emission (step 10 of the dispatch ladder):
            // lookup_builtin, overload pattern, spread reorder, per-arg
            // visit loop, out() special-casing, multi-buffer detection and
            // the per-shape emission tails — call_dispatch.cpp.
            return emit_builtin_call(node, n, func_name, call_loc,
                                     expanded_args, did_spread_swap);
        }

        case NodeType::Hole: {
            // Inside a loop() body the hole is the running accumulator,
            // resolved to the loop's running buffer (see handle_loop). The
            // analyzer deliberately leaves these holes un-substituted.
            if (!loop_hole_stack_.empty()) {
                return cache_and_return(node, loop_hole_stack_.back());
            }
            // Elsewhere holes should have been substituted by the analyzer.
            error("E110", "Hole '@' in unexpected context", n.location);
            return TypedValue::error_val();
        }

        case NodeType::Block: {
            // Visit all statements in block
            NodeIndex child = n.first_child;
            TypedValue last = TypedValue::void_val();
            while (child != NULL_NODE) {
                last = visit(child);
                child = ast_->arena[child].next_sibling;
            }
            return cache_and_return(node, last);
        }

        // Unsupported for MVP
        case NodeType::Pipe:
            error("E111", "Pipe should have been rewritten", n.location);
            return TypedValue::error_val();

        case NodeType::Closure:
            return handle_closure(node, n);

        case NodeType::MiniLiteral:
            return handle_mini_literal(node, n);

        case NodeType::FunctionDef:
            // Function definitions don't generate code directly
            // They're registered in the symbol table for inline expansion
            return TypedValue::void_val();

        case NodeType::MatchExpr:
            return handle_match_expr(node, n);

        case NodeType::LoopExpr:
            return handle_loop(node, n);

        case NodeType::MatchArm:
            // MatchArm nodes are handled by MatchExpr, not visited directly
            error("E122", "Match arm visited outside of match expression", n.location);
            return TypedValue::error_val();

        case NodeType::RecordLit:
            return handle_record_literal(node, n);

        case NodeType::FieldAccess:
            return handle_field_access(node, n);

        case NodeType::FieldAssignment:
            return handle_field_assignment(node, n);

        case NodeType::PipeBinding:
            return handle_pipe_binding(node, n);

        case NodeType::ImportDecl:
            // No-op: imports are resolved by the scanner before compilation
            return TypedValue::void_val();

        case NodeType::Directive:
            return handle_directive(node, n);

        default:
            error("E199", "Unsupported node type", n.location);
            return TypedValue::error_val();
    }
}

// handle_user_function_call, handle_closure, handle_match_expr are in codegen_functions.cpp
// handle_pattern_reference is in codegen_patterns.cpp
// handle_len_call is in codegen_arrays.cpp
// handle_chord_call is in codegen_patterns.cpp

// ============================================================================
// Directive handling
// ============================================================================

TypedValue CodeGenerator::handle_directive(NodeIndex node, const Node& n) {
    const auto& dir_data = n.as_directive();
    const std::string& dir_name = dir_data.name;

    if (dir_name == "polyphony") {
        // $polyphony(n) - set default voice count
        NodeIndex arg = n.first_child;
        if (arg == NULL_NODE) {
            error("E150", "$polyphony requires an argument", n.location);
            return TypedValue::void_val();
        }

        const Node& arg_node = ast_->arena[arg];
        if (arg_node.type != NodeType::NumberLit) {
            error("E151", "$polyphony argument must be a number literal", n.location);
            return TypedValue::void_val();
        }

        int value = static_cast<int>(arg_node.as_number());
        if (value < 1 || value > 32) {
            error("E152", "$polyphony value must be between 1 and 32", n.location);
            return TypedValue::void_val();
        }

        options_.default_polyphony = static_cast<std::uint8_t>(value);
    } else if (dir_name == "soundfont_alias") {
        // $soundfont_alias("name", "path") - bind a SoundFont alias resolved
        // at codegen time by sf_voice() / soundfont(). Must appear before the
        // calls that use it (like $polyphony).
        auto string_child = [&](NodeIndex arg) -> std::optional<std::string> {
            if (arg == NULL_NODE) return std::nullopt;
            NodeIndex value = arg;
            if (ast_->arena[value].type == NodeType::Argument) {
                value = ast_->arena[value].first_child;
            }
            if (value == NULL_NODE) return std::nullopt;
            const Node& vn = ast_->arena[value];
            if (vn.type != NodeType::StringLit) return std::nullopt;
            return vn.as_string();
        };

        NodeIndex name_arg = n.first_child;
        NodeIndex path_arg = (name_arg != NULL_NODE)
            ? ast_->arena[name_arg].next_sibling : NULL_NODE;
        auto alias_name = string_child(name_arg);
        auto alias_path = string_child(path_arg);
        if (!alias_name.has_value() || !alias_path.has_value()) {
            error("E153", "$soundfont_alias requires two string arguments: "
                  "$soundfont_alias(\"name\", \"path\")", n.location);
            return cache_and_return(node, TypedValue::void_val());
        }
        soundfont_aliases_[*alias_name] = *alias_path;
    } else {
        warn("W150", "Unknown directive '$" + dir_name + "'", n.location);
    }

    // Directives don't produce values
    return cache_and_return(node, TypedValue::void_val());
}

} // namespace akkado
