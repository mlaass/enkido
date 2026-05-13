#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"  // Master include for all codegen helpers
#include "akkado/builtins.hpp"
#include "akkado/source_map.hpp"
#include "akkado/stdlib.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/const_eval.hpp"
#include "akkado/pattern_eval.hpp"
#include <cedar/vm/state_pool.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace akkado {

// Use helpers from akkado::codegen namespace
using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::is_audio_rate_producer;
using codegen::is_upgradeable_oscillator;
using codegen::upgrade_for_fm;
using codegen::SamplePatternEmitCtx;
using codegen::emit_sample_chain;

std::uint16_t BufferAllocator::allocate() {
    if (next_ >= MAX_ALLOCATABLE) {
        return BUFFER_UNUSED;
    }
    return next_++;
}

void CodeGenerator::emit_extended_params_init(std::uint32_t state_id,
                                              const BuiltinInfo& info,
                                              const std::vector<std::uint16_t>& arg_buffers) {
    if (info.extended_param_count == 0) return;

    const std::size_t base = info.total_params();
    StateInitData ext{};
    // ExtendedParams lives in a sibling StatePool slot keyed off the
    // opcode's state_id XOR'd with EXT_PARAMS_STATE_XOR. This keeps the
    // opcode's primary DSP state (e.g. ChorusState) and its ExtendedParams
    // in distinct slots — get_or_create<DSPState> would otherwise overwrite
    // the ExtendedParams.
    ext.state_id = cedar::ext_params_state_id(state_id);
    ext.type = StateInitData::Type::ExtendedParams;
    ext.ext_count = info.extended_param_count;
    ext.ext_buffer_indices.fill(static_cast<std::uint16_t>(0xFFFF));

    for (std::uint8_t i = 0; i < info.extended_param_count && i < MAX_EXTENDED_PARAMS; ++i) {
        const std::size_t arg_idx = base + i;
        if (arg_idx < arg_buffers.size() &&
            arg_buffers[arg_idx] != BufferAllocator::BUFFER_UNUSED) {
            // Caller supplied an argument — read from its buffer at runtime.
            ext.ext_buffer_indices[i] = arg_buffers[arg_idx];
            ext.ext_constants[i] = 0.0f;
        } else if (i < info.extended_param_count &&
                   !std::isnan(info.extended_defaults[i])) {
            // Use the declared default as a constant slot.
            ext.ext_constants[i] = info.extended_defaults[i];
            ext.ext_buffer_indices[i] = 0xFFFFu;
        } else {
            // Required ext param missing — emit zero constant. The analyzer
            // is responsible for surfacing the missing-arg error upstream.
            ext.ext_constants[i] = 0.0f;
            ext.ext_buffer_indices[i] = 0xFFFFu;
        }
    }

    state_inits_.push_back(std::move(ext));
}

CodeGenResult CodeGenerator::generate(const Ast& ast, SymbolTable& symbols,
                                       std::string_view filename,
                                       SampleRegistry* sample_registry,
                                       const SourceMap* source_map) {
    ast_ = &ast;
    symbols_ = &symbols;
    sample_registry_ = sample_registry;
    source_map_ = source_map;
    buffers_ = BufferAllocator{};
    instructions_.clear();
    source_locations_.clear();
    diagnostics_.clear();
    state_inits_.clear();
    pre_resolved_values_.clear();
    required_samples_.clear();
    required_samples_extended_keys_.clear();
    required_samples_extended_.clear();
    scalar_sample_mappings_.clear();
    required_soundfonts_.clear();
    required_wavetables_.clear();
    required_uris_.clear();
    required_input_sources_.clear();
    param_decls_.clear();
    viz_decls_.clear();
    builtin_var_overrides_.clear();
    filename_ = std::string(filename);
    path_stack_.clear();
    anonymous_counter_ = 0;
    node_types_.clear();
    call_counters_.clear();
    param_function_refs_.clear();
    polyphonic_pattern_nodes_.clear();
    current_source_loc_ = {};
    options_ = CompilerOptions{};  // Reset compiler options

    // Start with "main" path
    push_path("main");

    if (!ast.valid()) {
        error("E100", "Invalid AST", {});
        return {{}, {}, std::move(diagnostics_), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, false};
    }

    // Visit root (Program node)
    visit(ast.root);

    pop_path();

    // Emit errors for polyphonic patterns not consumed by poly()
    for (const auto& [node, info] : polyphonic_pattern_nodes_) {
        error("E410", "Chord pattern has " + std::to_string(info.max_voices) +
              " voices but is not wrapped in poly(). "
              "Use poly(" + std::to_string(info.max_voices) +
              ", instrument_fn) to enable polyphonic playback.", info.location);
    }

    bool success = !has_errors(diagnostics_);

    // Convert required_samples set to vector
    std::vector<std::string> required_samples_vec(required_samples_.begin(), required_samples_.end());

    return {std::move(instructions_), std::move(source_locations_), std::move(diagnostics_),
            std::move(state_inits_), std::move(required_samples_vec), std::move(required_samples_extended_),
            std::move(scalar_sample_mappings_),
            std::move(required_soundfonts_), std::move(required_input_sources_),
            std::move(param_decls_), std::move(viz_decls_), std::move(builtin_var_overrides_),
            std::move(required_wavetables_), std::move(required_uris_), success};
}

void CodeGenerator::publish_sample_refs(const std::vector<RequiredSample>& refs) {
    for (const auto& r : refs) {
        std::string key = r.key();
        if (required_samples_extended_keys_.insert(key).second) {
            required_samples_extended_.push_back(r);
        }
    }
}

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

    // Synthetic PreResolved node: TypedValue is in pre_resolved_values_ side
    // table (populated during call-arg spread expansion). Returns the cached
    // value with no instruction emission — the underlying buffer was already
    // allocated when the spread source was first visited.
    if (n.type == NodeType::PreResolved) {
        auto pr_it = pre_resolved_values_.find(node);
        if (pr_it != pre_resolved_values_.end()) {
            return pr_it->second;
        }
        // Defensive: missing side-table entry shouldn't happen.
        return TypedValue::error_val();
    }

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
            // Emit PUSH_CONST
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }

            cedar::Instruction inst{};
            inst.opcode = cedar::Opcode::PUSH_CONST;
            inst.out_buffer = out;
            inst.inputs[0] = 0xFFFF;
            inst.inputs[1] = 0xFFFF;
            inst.inputs[2] = 0xFFFF;
            inst.inputs[3] = 0xFFFF;

            // Encode float value (split across inputs[4] and state_id)
            float value = static_cast<float>(n.as_number());
            encode_const_value(inst, value);

            emit(inst);
            return cache_and_return(node, TypedValue::number(out));
        }

        case NodeType::BoolLit: {
            // Emit PUSH_CONST with 1.0 or 0.0
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }

            cedar::Instruction inst{};
            inst.opcode = cedar::Opcode::PUSH_CONST;
            inst.out_buffer = out;
            inst.inputs[0] = 0xFFFF;
            inst.inputs[1] = 0xFFFF;
            inst.inputs[2] = 0xFFFF;
            inst.inputs[3] = 0xFFFF;

            float value = n.as_bool() ? 1.0f : 0.0f;
            encode_const_value(inst, value);

            emit(inst);
            return cache_and_return(node, TypedValue::number(out));
        }

        case NodeType::PitchLit: {
            // Emit PUSH_CONST for MIDI note, then MTOF to convert to frequency
            float midi_value = static_cast<float>(n.as_pitch());
            std::uint16_t freq_buf = codegen::emit_midi_to_freq(
                buffers_, instructions_, midi_value);
            if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }
            return cache_and_return(node, TypedValue::signal(freq_buf));
        }

        case NodeType::ChordLit: {
            // For MVP, emit first note (root) of chord
            // Full chord expansion would require array support
            const auto& chord = n.as_chord();
            float midi_value = static_cast<float>(chord.root_midi);
            std::uint16_t freq_buf = codegen::emit_midi_to_freq(
                buffers_, instructions_, midi_value);
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
                std::uint16_t out = buffers_.allocate();
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    return TypedValue::error_val();
                }
                cedar::Instruction inst{};
                inst.opcode = cedar::Opcode::PUSH_CONST;
                inst.out_buffer = out;
                inst.inputs[0] = 0xFFFF;
                inst.inputs[1] = 0xFFFF;
                inst.inputs[2] = 0xFFFF;
                inst.inputs[3] = 0xFFFF;
                encode_const_value(inst, 0.0f);
                emit(inst);
                return cache_and_return(node, TypedValue::make_array({}, out));
            }

            // Visit all elements, flattening any ..array spread elements.
            std::vector<TypedValue> elements;
            bool had_spread = false;
            NodeIndex elem = first_elem;
            while (elem != NULL_NODE) {
                const Node& en = ast_->arena[elem];
                if (en.type == NodeType::Argument &&
                    std::holds_alternative<Node::ArgumentData>(en.data) &&
                    en.as_argument().spread_source != NULL_NODE) {
                    had_spread = true;
                    NodeIndex src_node = en.as_argument().spread_source;
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
                std::uint16_t out = buffers_.allocate();
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    return TypedValue::error_val();
                }
                cedar::Instruction inst{};
                inst.opcode = cedar::Opcode::PUSH_CONST;
                inst.out_buffer = out;
                inst.inputs[0] = 0xFFFF;
                inst.inputs[1] = 0xFFFF;
                inst.inputs[2] = 0xFFFF;
                inst.inputs[3] = 0xFFFF;
                encode_const_value(inst, 0.0f);
                emit(inst);
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
                std::uint16_t out = buffers_.allocate();
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    return TypedValue::error_val();
                }

                cedar::Instruction unpack_inst{};
                unpack_inst.opcode = cedar::Opcode::ARRAY_UNPACK;
                unpack_inst.out_buffer = out;
                unpack_inst.inputs[0] = arr_tv.buffer;
                unpack_inst.inputs[1] = 0xFFFF;
                unpack_inst.inputs[2] = 0xFFFF;
                unpack_inst.inputs[3] = 0xFFFF;
                unpack_inst.inputs[4] = 0xFFFF;
                unpack_inst.rate = static_cast<std::uint8_t>(idx_val);
                unpack_inst.state_id = 0;
                emit(unpack_inst);

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
                std::uint16_t packed_buf = buffers_.allocate();
                if (packed_buf == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    return TypedValue::error_val();
                }

                // Pack first 5 elements
                cedar::Instruction pack_inst{};
                pack_inst.opcode = cedar::Opcode::ARRAY_PACK;
                pack_inst.out_buffer = packed_buf;
                std::uint8_t pack_count = std::min(arr_len, static_cast<std::uint8_t>(5));
                pack_inst.rate = pack_count;
                for (std::uint8_t i = 0; i < 5; ++i) {
                    pack_inst.inputs[i] = (i < pack_count) ? buffers[i] : 0xFFFF;
                }
                pack_inst.state_id = 0;
                emit(pack_inst);

                // For arrays > 5 elements, we need to pack remaining with ARRAY_PUSH
                for (std::uint8_t i = 5; i < arr_len; ++i) {
                    std::uint16_t new_packed = buffers_.allocate();
                    if (new_packed == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        return TypedValue::error_val();
                    }

                    cedar::Instruction push_inst{};
                    push_inst.opcode = cedar::Opcode::ARRAY_PUSH;
                    push_inst.out_buffer = new_packed;
                    push_inst.inputs[0] = packed_buf;
                    push_inst.inputs[1] = buffers[i];
                    push_inst.inputs[2] = 0xFFFF;
                    push_inst.inputs[3] = 0xFFFF;
                    push_inst.inputs[4] = 0xFFFF;
                    push_inst.rate = i;  // Current length before push
                    push_inst.state_id = 0;
                    emit(push_inst);

                    packed_buf = new_packed;
                }

                arr_buf = packed_buf;
            }

            // Now emit ARRAY_INDEX for dynamic per-sample indexing
            TypedValue idx_tv = visit(idx_node);
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }

            // Create a constant buffer with the array length for ARRAY_INDEX
            std::uint16_t len_buf = buffers_.allocate();
            if (len_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }

            cedar::Instruction len_inst{};
            len_inst.opcode = cedar::Opcode::PUSH_CONST;
            len_inst.out_buffer = len_buf;
            len_inst.inputs[0] = 0xFFFF;
            len_inst.inputs[1] = 0xFFFF;
            len_inst.inputs[2] = 0xFFFF;
            len_inst.inputs[3] = 0xFFFF;
            encode_const_value(len_inst, static_cast<float>(arr_len > 0 ? arr_len : 1));
            emit(len_inst);

            cedar::Instruction index_inst{};
            index_inst.opcode = cedar::Opcode::ARRAY_INDEX;
            index_inst.out_buffer = out;
            index_inst.inputs[0] = arr_buf;
            index_inst.inputs[1] = idx_tv.buffer;
            index_inst.inputs[2] = len_buf;  // Array length
            index_inst.inputs[3] = 0xFFFF;
            index_inst.inputs[4] = 0xFFFF;
            index_inst.rate = 0;  // 0 = wrap mode (default), 1 = clamp mode
            index_inst.state_id = 0;
            emit(index_inst);

            return cache_and_return(node, TypedValue::signal(out));
        }

        case NodeType::Identifier: {
            const std::string& name = n.as_identifier();

            // Builtin variable read (bpm, sr) — desugar to ENV_GET
            {
                auto bv_it = BUILTIN_VARIABLES.find(name);
                if (bv_it != BUILTIN_VARIABLES.end()) {
                    const auto& bv = bv_it->second;
                    std::uint32_t key_hash = cedar::fnv1a_hash_runtime(
                        bv.env_key.data(), bv.env_key.size());

                    // Emit PUSH_CONST for default/fallback value
                    std::uint16_t fallback_buf = buffers_.allocate();
                    if (fallback_buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        return TypedValue::error_val();
                    }
                    cedar::Instruction push_inst{};
                    push_inst.opcode = cedar::Opcode::PUSH_CONST;
                    push_inst.out_buffer = fallback_buf;
                    push_inst.inputs[0] = 0xFFFF;
                    push_inst.inputs[1] = 0xFFFF;
                    push_inst.inputs[2] = 0xFFFF;
                    push_inst.inputs[3] = 0xFFFF;
                    encode_const_value(push_inst, bv.default_value);
                    emit(push_inst);

                    // Emit ENV_GET with reserved key hash
                    std::uint16_t out_buf = buffers_.allocate();
                    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        return TypedValue::error_val();
                    }
                    cedar::Instruction env_inst{};
                    env_inst.opcode = cedar::Opcode::ENV_GET;
                    env_inst.out_buffer = out_buf;
                    env_inst.inputs[0] = fallback_buf;
                    env_inst.inputs[1] = 0xFFFF;
                    env_inst.inputs[2] = 0xFFFF;
                    env_inst.inputs[3] = 0xFFFF;
                    env_inst.inputs[4] = 0xFFFF;
                    env_inst.state_id = key_hash;
                    emit(env_inst);

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
                    // Empty rest → zero
                    auto zero_buf = buffers_.allocate();
                    cedar::Instruction push_zero{};
                    push_zero.opcode = cedar::Opcode::PUSH_CONST;
                    push_zero.out_buffer = zero_buf;
                    push_zero.inputs[0] = 0xFFFF;
                    push_zero.inputs[1] = 0xFFFF;
                    push_zero.inputs[2] = 0xFFFF;
                    push_zero.inputs[3] = 0xFFFF;
                    codegen::encode_const_value(push_zero, 0.0f);
                    emit(push_zero);
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

            const std::string& var_name = n.as_identifier();

            // Builtin variable assignment (bpm = 120) — extract compile-time constant
            {
                auto bv_it = BUILTIN_VARIABLES.find(var_name);
                if (bv_it != BUILTIN_VARIABLES.end()) {
                    const auto& bv = bv_it->second;
                    if (bv.setter_name.empty()) {
                        error("E170", "Cannot assign to read-only builtin variable '" +
                              std::string(var_name) + "'", n.location);
                        return TypedValue::error_val();
                    }
                    // Evaluate RHS as compile-time constant
                    ConstEvaluator evaluator(*ast_, *symbols_);
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
                    new_sym.name_hash = fnv1a_hash(var_name);
                    new_sym.buffer_index = value_tv.buffer;
                    new_sym.multi_buffers = buffers_of(value_tv);
                    new_sym.typed_value = value_tv;
                    symbols_->define(new_sym);
                } else if (value_tv.type == ValueType::Record ||
                           value_tv.type == ValueType::Pattern ||
                           value_tv.type == ValueType::StateCell) {
                    // Preserve rich type info through symbol table.
                    // StateCell carries cell_state_id which get/set need to
                    // route the STATE_OP instruction to the right slot.
                    Symbol new_sym;
                    new_sym.kind = SymbolKind::Variable;
                    new_sym.name = var_name;
                    new_sym.name_hash = fnv1a_hash(var_name);
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
            const auto& dd = n.as_destructure_assignment();
            NodeIndex value_idx = n.first_child;
            if (value_idx == NULL_NODE) {
                error("E104", "Invalid destructure assignment", n.location);
                return TypedValue::error_val();
            }
            TypedValue value_tv = visit(value_idx);
            if (value_tv.error) {
                return cache_and_return(node, TypedValue::error_val());
            }
            bind_destructure_fields(value_tv, dd.fields, n.location, "E187");
            return cache_and_return(node, TypedValue::void_val());
        }

        case NodeType::ConstDecl: {
            // Const variable: evaluate RHS at compile time
            const std::string& var_name = n.as_identifier();
            NodeIndex value_idx = n.first_child;

            if (value_idx == NULL_NODE) {
                error("E104", "Invalid const declaration", n.location);
                return TypedValue::error_val();
            }

            // Evaluate at compile time using ConstEvaluator
            ConstEvaluator evaluator(*ast_, *symbols_);
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
                std::uint16_t buf = codegen::emit_push_const(buffers_, instructions_, val);
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
                source_locations_.push_back(n.location);
                return cache_and_return(node, TypedValue::number(buf));
            } else {
                // Array const value
                const auto& arr = std::get<std::vector<double>>(*const_val);
                std::vector<TypedValue> result_elements;
                for (double v : arr) {
                    std::uint16_t buf = codegen::emit_push_const(buffers_, instructions_,
                                                                  static_cast<float>(v));
                    if (buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        return TypedValue::error_val();
                    }
                    source_locations_.push_back(n.location);
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
                    sym.name_hash = fnv1a_hash(var_name);
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
            const std::string& func_name = n.as_identifier();

            // Save the call's source location - visiting arguments may overwrite it
            SourceLocation call_loc = current_source_loc_;

            // Check user-defined functions FIRST (allows stdlib osc to work)
            auto sym = symbols_->lookup(func_name);
            if (sym && sym->kind == SymbolKind::UserFunction) {
                return handle_user_function_call(node, n, sym->user_function);
            }

            // Check for FunctionValue (lambda assigned to variable)
            if (sym && sym->kind == SymbolKind::FunctionValue) {
                return handle_function_value_call(node, n, sym->function_ref);
            }

            // Phase 6: builtin/special-handler spread expansion. If the call
            // contains ..record / ..array spread args, materialize the
            // expanded list as a synthetic Argument chain (with PreResolved
            // children for spread-resolved values) and swap it into the call
            // node before dispatch. RAII restores on exit.
            NodeIndex saved_first_child = NULL_NODE;
            bool did_spread_swap = false;
            {
                NodeIndex it = n.first_child;
                bool has_spread = false;
                while (it != NULL_NODE) {
                    const Node& a = ast_->arena[it];
                    if (a.type == NodeType::Argument &&
                        std::holds_alternative<Node::ArgumentData>(a.data) &&
                        a.as_argument().spread_source != NULL_NODE) {
                        has_spread = true;
                        break;
                    }
                    it = ast_->arena[it].next_sibling;
                }
                if (has_spread) {
                    auto expanded_opt = expand_call_arguments(node);
                    if (!expanded_opt) return TypedValue::error_val();

                    AstArena& arena = const_cast<AstArena&>(ast_->arena);
                    NodeIndex chain_head = NULL_NODE;
                    NodeIndex prev = NULL_NODE;
                    for (const auto& ea : *expanded_opt) {
                        NodeIndex arg_idx = arena.alloc(NodeType::Argument, ea.loc);
                        arena[arg_idx].data = Node::ArgumentData{ea.name, NULL_NODE};

                        NodeIndex child_idx;
                        if (ea.resolved) {
                            child_idx = arena.alloc(NodeType::PreResolved, ea.loc);
                            pre_resolved_values_[child_idx] = *ea.resolved;
                        } else {
                            child_idx = ea.source_node;
                        }
                        arena[arg_idx].first_child = child_idx;
                        // Note: child_idx may have its own next_sibling (from
                        // the original AST). Don't overwrite that — the
                        // Argument-as-parent semantics treat first_child as
                        // the value, and its sibling chain is irrelevant once
                        // wrapped (each Argument has exactly one value).
                        // But since AstArena stores siblings in-band, we
                        // must defensively detach. Cloning avoids surprises;
                        // here we simply clear next_sibling of the wrapped
                        // child's NEW reference. Original AST already used
                        // next_sibling to chain its parent's children — that
                        // was needed only at the original parent (the call
                        // node), and the value node's next_sibling was
                        // already NULL_NODE by parser construction.

                        if (prev == NULL_NODE) {
                            chain_head = arg_idx;
                        } else {
                            arena[prev].next_sibling = arg_idx;
                        }
                        prev = arg_idx;
                    }

                    Node& mut_n = const_cast<Node&>(n);
                    saved_first_child = mut_n.first_child;
                    mut_n.first_child = chain_head;
                    did_spread_swap = true;
                }
            }

            // RAII: restore the call's child chain on every return path.
            struct ChildRestore {
                Node* node;
                NodeIndex orig;
                bool active;
                ~ChildRestore() {
                    if (active) node->first_child = orig;
                }
            } child_restore{
                did_spread_swap ? const_cast<Node*>(&n) : nullptr,
                saved_first_child,
                did_spread_swap
            };

            // Dispatch table for special function handlers
            using Handler = TypedValue (CodeGenerator::*)(NodeIndex, const Node&);
            static const std::unordered_map<std::string_view, Handler> special_handlers = {
                {"len",     &CodeGenerator::handle_len_call},
                // User state cells (Phase 3 of userspace-state PRD). state/get/set
                // are reserved at parser level — no user closure can shadow them.
                {"state",   &CodeGenerator::handle_state_call},
                {"get",     &CodeGenerator::handle_get_call},
                {"set",     &CodeGenerator::handle_set_call},
                {"chord",   &CodeGenerator::handle_chord_call},
                // PRD prd-patterns-as-scalar-values: typed-prefix call forms.
                {"value",   &CodeGenerator::handle_value_call},
                {"note",    &CodeGenerator::handle_note_call},
                {"scalar",  &CodeGenerator::handle_scalar_call},
                {"map",     &CodeGenerator::handle_map_call},
                {"sum",     &CodeGenerator::handle_sum_call},
                {"reduce",  &CodeGenerator::handle_reduce_call},
                {"zipWith", &CodeGenerator::handle_zipWith_call},
                {"zip",     &CodeGenerator::handle_zip_call},
                {"take",    &CodeGenerator::handle_take_call},
                {"drop",    &CodeGenerator::handle_drop_call},
                {"reverse", &CodeGenerator::handle_reverse_call},
                {"range",   &CodeGenerator::handle_range_call},
                {"repeat",  &CodeGenerator::handle_repeat_call},
                {"spread",  &CodeGenerator::handle_spread_call},
                // Timeline curve call form
                {"timeline",  &CodeGenerator::handle_timeline_call},
                // Pattern transformation builtins
                {"slow",      &CodeGenerator::handle_slow_call},
                {"fast",      &CodeGenerator::handle_fast_call},
                {"rev",       &CodeGenerator::handle_rev_call},
                {"transpose", &CodeGenerator::handle_transpose_call},
                {"velocity",  &CodeGenerator::handle_velocity_call},
                // Phase 2.1 PRD §11.2: standalone note-property transforms
                {"bend",        &CodeGenerator::handle_bend_call},
                {"aftertouch",  &CodeGenerator::handle_aftertouch_call},
                {"dur",         &CodeGenerator::handle_dur_call},
                {"bank",      &CodeGenerator::handle_bank_call},
                {"variant",   &CodeGenerator::handle_variant_call},
                {"transport", &CodeGenerator::handle_transport_call},
                {"tune",      &CodeGenerator::handle_tune_call},
                // Phase 2 PRD time/structure transforms
                {"early",      &CodeGenerator::handle_early_call},
                {"late",       &CodeGenerator::handle_late_call},
                {"palindrome", &CodeGenerator::handle_palindrome_call},
                {"compress",   &CodeGenerator::handle_compress_call},
                {"ply",        &CodeGenerator::handle_ply_call},
                {"linger",     &CodeGenerator::handle_linger_call},
                {"zoom",       &CodeGenerator::handle_zoom_call},
                {"segment",    &CodeGenerator::handle_segment_call},
                {"swing",      &CodeGenerator::handle_swing_call},
                {"swingBy",    &CodeGenerator::handle_swing_by_call},
                {"iter",       &CodeGenerator::handle_iter_call},
                {"iterBack",   &CodeGenerator::handle_iter_back_call},
                // Phase 2 PRD generators
                {"run",        &CodeGenerator::handle_run_call},
                {"binary",     &CodeGenerator::handle_binary_call},
                {"binaryN",    &CodeGenerator::handle_binary_n_call},
                // Phase 2 PRD voicing
                {"anchor",     &CodeGenerator::handle_anchor_call},
                {"mode",       &CodeGenerator::handle_mode_call},
                {"voicing",    &CodeGenerator::handle_voicing_call},
                {"addVoicings", &CodeGenerator::handle_add_voicings_call},
                // Parameter exposure builtins
                {"param",   &CodeGenerator::handle_param_call},
                {"button",  &CodeGenerator::handle_button_call},
                {"toggle",  &CodeGenerator::handle_toggle_call},
                {"dropdown", &CodeGenerator::handle_select_call},
                // Array reduction operations
                {"mean",    &CodeGenerator::handle_mean_call},
                // Array transformation operations
                {"rotate",    &CodeGenerator::handle_rotate_call},
                {"shuffle",   &CodeGenerator::handle_shuffle_call},
                {"sort",      &CodeGenerator::handle_sort_call},
                {"normalize", &CodeGenerator::handle_normalize_call},
                {"scale",     &CodeGenerator::handle_scale_call},
                // Array generation operations
                {"linspace",  &CodeGenerator::handle_linspace_call},
                {"random",    &CodeGenerator::handle_random_call},
                {"harmonics", &CodeGenerator::handle_harmonics_call},
                // Binary operation broadcasting (desugared from +, -, *, /, ^)
                {"add",     &CodeGenerator::handle_binary_op_call},
                {"sub",     &CodeGenerator::handle_binary_op_call},
                {"mul",     &CodeGenerator::handle_binary_op_call},
                {"div",     &CodeGenerator::handle_binary_op_call},
                {"pow",     &CodeGenerator::handle_binary_op_call},
                // min/max with array support
                {"min",     &CodeGenerator::handle_minmax_call},
                {"max",     &CodeGenerator::handle_minmax_call},
                // Tap delay with configurable feedback chain (all time unit variants)
                {"tap_delay", &CodeGenerator::handle_tap_delay_call},
                {"tap_delay_ms", &CodeGenerator::handle_tap_delay_call},
                {"tap_delay_smp", &CodeGenerator::handle_tap_delay_call},
                // Stereo operations
                {"stereo", &CodeGenerator::handle_stereo_call},
                {"left", &CodeGenerator::handle_left_call},
                {"right", &CodeGenerator::handle_right_call},
                {"pan", &CodeGenerator::handle_pan_call},
                {"width", &CodeGenerator::handle_width_call},
                {"ms_encode", &CodeGenerator::handle_ms_encode_call},
                {"ms_decode", &CodeGenerator::handle_ms_decode_call},
                {"pingpong", &CodeGenerator::handle_pingpong_call},
                // Visualization builtins
                {"pianoroll", &CodeGenerator::handle_pianoroll_call},
                {"oscilloscope", &CodeGenerator::handle_oscilloscope_call},
                {"waveform", &CodeGenerator::handle_waveform_call},
                {"spectrum", &CodeGenerator::handle_spectrum_call},
                {"waterfall", &CodeGenerator::handle_waterfall_call},
                // Function composition
                {"compose", &CodeGenerator::handle_compose_call},
                // SoundFont playback
                {"soundfont", &CodeGenerator::handle_soundfont_call},
                // Wavetable: wt_load(name, path) is a compile-time directive
                // (records required_wavetables, no instruction emitted).
                // smooch / wt / wavetable resolve the bank name to its ID at
                // compile time and emit OSC_WAVETABLE with rate = bank_id.
                {"wt_load",   &CodeGenerator::handle_wt_load_call},
                // samples("uri") — compile-time URI declaration for sample
                // banks; records to required_uris_ for host to fetch.
                {"samples",   &CodeGenerator::handle_samples_call},
                {"smooch",    &CodeGenerator::handle_smooch_call},
                {"wt",        &CodeGenerator::handle_smooch_call},
                {"wavetable", &CodeGenerator::handle_smooch_call},
                // Live audio input (microphone / tab / file)
                {"in", &CodeGenerator::handle_input_call},
                // Polyphony
                {"poly",   &CodeGenerator::handle_poly_call},
                {"mono",   &CodeGenerator::handle_mono_call},
                {"legato", &CodeGenerator::handle_poly_call},
            };

            auto handler_it = special_handlers.find(func_name);
            if (handler_it != special_handlers.end()) {
                TypedValue tv = (this->*(handler_it->second))(node, n);
                // Most handlers cache via cache_and_return on success, but
                // several pattern-transform handlers (handle_fast_call et al.)
                // leave error paths uncached. Without this defensive write a
                // re-visit (e.g. the channel-type check below at the
                // !auto_lift loop) cache-misses, re-runs the handler, and
                // duplicates its diagnostics.
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
                        std::uint16_t freq_buf = buffers_.allocate();
                        if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            return TypedValue::error_val();
                        }

                        cedar::Instruction mtof_inst{};
                        mtof_inst.opcode = cedar::Opcode::MTOF;
                        mtof_inst.out_buffer = freq_buf;
                        mtof_inst.inputs[0] = mb;
                        mtof_inst.inputs[1] = 0xFFFF;
                        mtof_inst.inputs[2] = 0xFFFF;
                        mtof_inst.inputs[3] = 0xFFFF;
                        mtof_inst.state_id = 0;
                        emit(mtof_inst);

                        freq_elements.push_back(TypedValue::signal(freq_buf));
                    }

                    std::uint16_t first_buf = freq_elements[0].buffer;
                    auto tv = TypedValue::make_array(std::move(freq_elements), first_buf);
                    return cache_and_return(node, tv);
                }

                // Single buffer case - fall through to normal handling
            }

            const BuiltinInfo* builtin = lookup_builtin(func_name);

            if (!builtin) {
                error("E107", "Unknown function: '" + func_name + "'", n.location);
                return TypedValue::error_val();
            }

            // For stateful functions, push path BEFORE visiting children
            // so nested calls see their parent's context
            bool pushed_path = false;
            if (builtin->requires_state) {
                std::uint32_t count = call_counters_[func_name]++;
                std::string unique_name = func_name + "#" + std::to_string(count);
                push_path(unique_name);
                pushed_path = true;
            }

            // Visit arguments (dependencies must be satisfied)
            std::vector<std::uint16_t> arg_buffers;
            std::size_t arg_idx = 0;
            NodeIndex arg = n.first_child;
            while (arg != NULL_NODE) {
                const Node& arg_node = ast_->arena[arg];
                NodeIndex arg_value = arg;
                if (arg_node.type == NodeType::Argument) {
                    arg_value = arg_node.first_child;
                }

                // Check for underscore placeholder: fill with default value
                const Node& val_node = ast_->arena[arg_value];
                bool is_placeholder = (val_node.type == NodeType::Identifier &&
                    std::holds_alternative<Node::IdentifierData>(val_node.data) &&
                    val_node.as_identifier() == "_");

                if (is_placeholder) {
                    if (builtin->has_default(arg_idx)) {
                        std::uint16_t default_buf = buffers_.allocate();
                        if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            if (pushed_path) pop_path();
                            return TypedValue::error_val();
                        }
                        cedar::Instruction push_inst{};
                        push_inst.opcode = cedar::Opcode::PUSH_CONST;
                        push_inst.out_buffer = default_buf;
                        push_inst.inputs[0] = 0xFFFF;
                        push_inst.inputs[1] = 0xFFFF;
                        push_inst.inputs[2] = 0xFFFF;
                        push_inst.inputs[3] = 0xFFFF;
                        encode_const_value(push_inst, builtin->get_default(arg_idx));
                        emit(push_inst);
                        arg_buffers.push_back(default_buf);
                    } else {
                        error("E106", "Cannot skip required parameter '" +
                              std::string(builtin->param_names[arg_idx]) +
                              "' — no default value", val_node.location);
                        arg_buffers.push_back(0);
                    }
                } else {
                    TypedValue arg_tv = visit(arg_value);

                    // sample("name"[, ...]) form: parse the sample-name string,
                    // register it for runtime loading, and emit a PUSH_CONST
                    // placeholder whose state_id immediate is patched to the
                    // bank-assigned sample ID after samples are loaded. The
                    // numeric `sample(t, p, <int>)` form falls through.
                    if (func_name == "sample" && arg_idx == 2 &&
                        arg_tv.type == ValueType::String &&
                        val_node.type == NodeType::StringLit) {
                        const std::string& raw = val_node.as_string();
                        std::string bank, after_bank, name;
                        int variant = 0;
                        auto slash = raw.find('/');
                        if (slash != std::string::npos) {
                            bank = raw.substr(0, slash);
                            after_bank = raw.substr(slash + 1);
                        } else {
                            after_bank = raw;
                        }
                        auto colon = after_bank.rfind(':');
                        if (colon != std::string::npos) {
                            std::string vstr = after_bank.substr(colon + 1);
                            bool all_digits = !vstr.empty() &&
                                std::all_of(vstr.begin(), vstr.end(),
                                    [](char c){ return std::isdigit(static_cast<unsigned char>(c)); });
                            if (all_digits) {
                                variant = std::stoi(vstr);
                                name = after_bank.substr(0, colon);
                            } else {
                                name = after_bank;
                            }
                        } else {
                            name = after_bank;
                        }

                        if (name.empty()) {
                            error("E161", "sample() name string is empty", val_node.location);
                        } else {
                            publish_sample_refs({RequiredSample{bank, name, variant}});

                            std::uint16_t buf = buffers_.allocate();
                            if (buf == BufferAllocator::BUFFER_UNUSED) {
                                error("E101", "Buffer pool exhausted", val_node.location);
                            } else {
                                cedar::Instruction push_inst{};
                                push_inst.opcode = cedar::Opcode::PUSH_CONST;
                                push_inst.out_buffer = buf;
                                push_inst.inputs[0] = 0xFFFF;
                                push_inst.inputs[1] = 0xFFFF;
                                push_inst.inputs[2] = 0xFFFF;
                                push_inst.inputs[3] = 0xFFFF;
                                encode_const_value(push_inst, 0.0f);

                                std::uint32_t inst_idx =
                                    static_cast<std::uint32_t>(instructions_.size());
                                scalar_sample_mappings_.push_back(
                                    ScalarSampleMapping{inst_idx, bank, name, variant});

                                emit(push_inst);
                                arg_tv = TypedValue::signal(buf);
                            }
                        }
                    }

                    // PRD prd-patterns-as-scalar-values §5.3: implicit
                    // Pattern→Signal coerce. arg_tv.buffer is already the
                    // pattern's primary value buffer — for monophonic non-
                    // sample patterns this is the FREQ buffer (Signal-typed
                    // since SEQPAT_STEP populates raw scalars); for sample
                    // patterns it is the post-SAMPLE_PLAY audio output
                    // (already a Signal), which legitimately routes through
                    // out(), gain stages, and effects.
                    //
                    // The genuine footgun is a polyphonic non-sample pattern
                    // (chord, multi-voice note pattern): without a coerce
                    // reject, `osc("sin", c"Am")` would silently emit only
                    // voice-0's freq, dropping the chord's other voices.
                    // Reject those at the slot with E160 so the user opts
                    // into poly() / scalar() / a voice index explicitly.
                    bool slot_expects_signal =
                        arg_idx < MAX_BUILTIN_PARAMS &&
                        (builtin->param_types[arg_idx] == ParamValueType::Signal ||
                         builtin->param_types[arg_idx] == ParamValueType::Any);
                    if (builtin->args_are_signal && slot_expects_signal &&
                        arg_tv.type == ValueType::Pattern && arg_tv.pattern &&
                        arg_tv.pattern->max_voices > 1 &&
                        !arg_tv.pattern->is_sample_pattern) {
                        error("E160",
                              func_name + "() cannot use a polyphonic pattern as scalar at "
                              "argument '" + std::string(builtin->param_names[arg_idx]) +
                              "'; use poly() to consume it, or pick a voice/field "
                              "explicitly (e.g. p.freq)",
                              ast_->arena[arg_value].location);
                    }

                    arg_buffers.push_back(arg_tv.buffer);

                    // Type check against annotation (non-fatal — continue for max error reporting)
                    if (arg_idx < MAX_BUILTIN_PARAMS &&
                        builtin->param_types[arg_idx] != ParamValueType::Any &&
                        !arg_tv.error && arg_tv.type != ValueType::Void) {
                        if (!type_compatible(arg_tv.type, builtin->param_types[arg_idx])) {
                            error("E160", func_name + "() argument '" +
                                  std::string(builtin->param_names[arg_idx]) + "' expects " +
                                  param_value_type_name(builtin->param_types[arg_idx]) +
                                  ", got " + value_type_name(arg_tv.type),
                                  ast_->arena[arg_value].location);
                        }
                    }
                }

                ++arg_idx;
                arg = ast_->arena[arg].next_sibling;
            }

            // PRD §4.4 / §5.3 rule 2: out(L, R) expects two Mono signals.
            // Any Stereo in either slot is a compile error — tell the user
            // exactly how to fix it (wrap the stereo in out(sig) or split
            // with left()/right()).
            //
            // Only flag when the argument's resolved TypedValue is stereo
            // (channels == Stereo with a valid right_buffer). Checking
            // `stereo_buffer_pairs_` would false-positive on `left(s)` /
            // `right(s)` extractions that legitimately reuse the pair's
            // left/right buffer as a mono signal.
            if (func_name == "out" && arg_buffers.size() == 2) {
                // PRD prd-stereo-native-opcodes §5.6, §9.4: out() argument
                // shape mismatch is no longer E185 — it auto-escalates.
                // For each arg, we route a (L, R) contribution to the
                // accumulating OUTPUT bus:
                //   - Mono arg  → broadcast (L = R = arg)
                //   - Stereo arg → split (L = arg.left, R = arg.right)
                // The two contributions sum at the bus since OUTPUT does
                // `output_left += L; output_right += R;` (utility.hpp:55-56).
                //
                // Detect a mismatch first so we can avoid the per-arg
                // emission path when both args are mono (the existing
                // single-OUTPUT emission below is unchanged for that case).
                bool any_stereo = false;
                std::array<TypedValue, 2> resolved{};
                {
                    NodeIndex ch = n.first_child;
                    for (std::size_t ai = 0; ai < 2 && ch != NULL_NODE; ++ai,
                             ch = ast_->arena[ch].next_sibling) {
                        const Node& arg_node = ast_->arena[ch];
                        NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                             arg_node.first_child : ch;
                        resolved[ai] = visit(arg_value);
                        if (resolved[ai].is_stereo() &&
                            resolved[ai].right_buffer != 0xFFFF) {
                            any_stereo = true;
                        }
                    }
                }
                if (any_stereo) {
                    warn("W185",
                         "out() with mixed channel shapes — auto-escalating: "
                         "mono args broadcast to both buses, stereo args drive "
                         "both buses; contributions sum.",
                         n.location);
                    for (std::size_t ai = 0; ai < 2; ++ai) {
                        std::uint16_t left_in  = arg_buffers[ai];
                        std::uint16_t right_in = arg_buffers[ai];
                        if (resolved[ai].is_stereo() &&
                            resolved[ai].right_buffer != 0xFFFF) {
                            left_in  = resolved[ai].buffer;
                            right_in = resolved[ai].right_buffer;
                        }
                        cedar::Instruction out_inst{};
                        out_inst.opcode = cedar::Opcode::OUTPUT;
                        out_inst.out_buffer = 0xFFFF;
                        out_inst.inputs[0] = left_in;
                        out_inst.inputs[1] = right_in;
                        out_inst.inputs[2] = 0xFFFF;
                        out_inst.inputs[3] = 0xFFFF;
                        out_inst.inputs[4] = 0xFFFF;
                        out_inst.state_id = 0;
                        emit(out_inst);
                    }
                    if (pushed_path) pop_path();
                    return cache_and_return(node, TypedValue::void_val());
                }
            }

            // Special case: out() with single argument
            // Check if the argument is stereo - if so, use both channels
            if (func_name == "out" && arg_buffers.size() == 1) {
                // Get the first argument node to check if it's stereo
                NodeIndex first_arg = n.first_child;
                if (first_arg != NULL_NODE) {
                    const Node& arg_node = ast_->arena[first_arg];
                    NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                         arg_node.first_child : first_arg;

                    // Check stereo by both node and buffer (buffer fallback for pipe chains)
                    bool arg_is_stereo = is_stereo(arg_value) || is_stereo_buffer(arg_buffers[0]);

                    if (arg_is_stereo) {
                        // Stereo input - use both channels
                        StereoBuffers stereo;
                        if (is_stereo(arg_value)) {
                            stereo = get_stereo_buffers(arg_value);
                        } else {
                            stereo = get_stereo_buffers_by_buffer(arg_buffers[0]);
                        }
                        arg_buffers[0] = stereo.left;
                        arg_buffers.push_back(stereo.right);
                    } else {
                        // Mono input - duplicate to both channels
                        arg_buffers.push_back(arg_buffers[0]);
                    }
                } else {
                    // No argument node? Just duplicate
                    arg_buffers.push_back(arg_buffers[0]);
                }
            }

            // PRD §5.3 rule 1 / §5.2 (G1): declarative channel-type mismatch.
            // For builtins that do not opt into auto-lift or stereo_native, a
            // stereo signal in a Mono slot (or vice versa) is a compile error
            // E186. Special-handler builtins (stereo/mono/left/right/pan/
            // width/ms_encode/ms_decode/pingpong) never reach this path —
            // they enforce their own signatures with E181–E184. `out()` is
            // handled above via E185 and the single-arg expansion branch.
            //
            // Stereo-native opcodes (prd-stereo-native-opcodes Phase 1) skip
            // this check because they auto-escalate mono → stereo at the
            // boundary (mono input is broadcast inside the opcode body).
            if (func_name != "out" && !builtin->auto_lift && !builtin->stereo_native) {
                NodeIndex ch = n.first_child;
                for (std::size_t ai = 0; ai < arg_buffers.size() &&
                        ai < MAX_BUILTIN_PARAMS && ch != NULL_NODE; ++ai,
                        ch = ast_->arena[ch].next_sibling) {
                    const Node& arg_node = ast_->arena[ch];
                    NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                         arg_node.first_child : ch;
                    TypedValue resolved = visit(arg_value);
                    if (resolved.type != ValueType::Signal) continue;
                    ChannelCount actual = (resolved.is_stereo() &&
                                          resolved.right_buffer != 0xFFFF)
                                          ? ChannelCount::Stereo
                                          : ChannelCount::Mono;
                    ChannelCount expected = builtin->input_channels[ai];
                    if (actual != expected) {
                        std::string param_label;
                        if (!builtin->param_names[ai].empty()) {
                            param_label = " '" + std::string(builtin->param_names[ai]) + "'";
                        }
                        std::string hint = (expected == ChannelCount::Mono)
                            ? " Use `left(x)`, `right(x)`, or `mono(x)` to reduce to mono."
                            : " Use `stereo(x)` to promote to stereo.";
                        error("E186",
                              "'" + std::string(func_name) +
                              "' expects " + channel_count_name(expected) +
                              " for argument" + param_label +
                              " (position " + std::to_string(ai + 1) + "), got " +
                              channel_count_name(actual) + "." + hint,
                              ast_->arena[ch].location);
                        if (pushed_path) pop_path();
                        return TypedValue::error_val();
                    }
                }
            }

            // Multi-buffer argument detection (stereo or chord expansion).
            // Stereo auto-lift runs regardless of `requires_state` — stateless
            // ops (fold, saturate, distort_tanh, ...) must also run twice on
            // stereo input per PRD §5.2. Chord expansion-to-N still only
            // applies to stateful UGens (per-voice state).
            int expansion_arg_idx = -1;
            std::vector<std::uint16_t> expansion_buffers;
            std::vector<NodeIndex> arg_nodes;
            bool is_stereo_expansion = false;

            if (!arg_buffers.empty()) {
                NodeIndex arg_iter = n.first_child;
                while (arg_iter != NULL_NODE) {
                    const Node& arg_node = ast_->arena[arg_iter];
                    NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                         arg_node.first_child : arg_iter;
                    arg_nodes.push_back(arg_value);
                    arg_iter = ast_->arena[arg_iter].next_sibling;
                }

                for (std::size_t i = 0; i < arg_nodes.size(); ++i) {
                    if (is_multi_buffer(arg_nodes[i])) {
                        expansion_arg_idx = static_cast<int>(i);
                        expansion_buffers = get_multi_buffers(arg_nodes[i]);
                        break;
                    }
                    const Node& arg_n = ast_->arena[arg_nodes[i]];
                    if (arg_n.type == NodeType::Identifier) {
                        const std::string& name = arg_n.as_identifier();
                        std::uint32_t param_hash = fnv1a_hash(name);
                        auto pit = param_multi_buffer_sources_.find(param_hash);
                        if (pit != param_multi_buffer_sources_.end()) {
                            NodeIndex source_node = pit->second;
                            if (is_multi_buffer(source_node)) {
                                expansion_arg_idx = static_cast<int>(i);
                                expansion_buffers = get_multi_buffers(source_node);
                                break;
                            }
                        }
                    }
                }

                // Stereo auto-lift is driven by the builtin's declarative
                // `auto_lift` flag (PRD §5.2, G1 — declarative BuiltinSignature
                // catalog). Builtins that don't opt in (oscillators, math,
                // non-lift operations) are NOT silently lifted on stereo input;
                // mismatches are caught by the E186 channel-type check
                // downstream. Chord expansion (>2 voices) still fires on its
                // own branch regardless of `auto_lift`.
                is_stereo_expansion = (builtin->auto_lift &&
                                      expansion_arg_idx >= 0 &&
                                      expansion_buffers.size() == 2 &&
                                      is_stereo(arg_nodes[static_cast<std::size_t>(expansion_arg_idx)]));
            }

            // PRD prd-stereo-native-opcodes Phase 1: declarative stereo-native
            // emission. Builtins that opt in produce a stereo output pair in a
            // single dispatch, regardless of input channel count (mono auto-
            // escalates to L=R inside the opcode body). Stereo primary input
            // adds STEREO_INPUT to the flag combo so the opcode reads
            // inputs[0]+1 for R. Chord/polyphonic expansion onto a stereo-
            // native opcode is rejected — user must wrap explicitly in poly().
            if (builtin->stereo_native) {
                bool primary_stereo =
                    expansion_arg_idx >= 0 &&
                    expansion_buffers.size() == 2 &&
                    is_stereo(arg_nodes[static_cast<std::size_t>(expansion_arg_idx)]);

                // Chord/poly expansion into stereo-native = E187. Stereo input
                // (size == 2 + is_stereo) is fine; >2 voices, or 2 voices that
                // aren't a stereo pair, indicate a chord that needs poly().
                bool is_chord_expansion =
                    expansion_arg_idx >= 0 &&
                    expansion_buffers.size() > 1 &&
                    !primary_stereo;
                if (is_chord_expansion) {
                    error("E187",
                          "Chord/polyphonic expansion into stereo-native opcode '" +
                          std::string(func_name) +
                          "' requires explicit poly() wrapping.",
                          call_loc);
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }

                current_source_loc_ = call_loc;

                std::size_t n_params = builtin->total_params();
                auto expanded_args = arg_buffers;

                // For stereo primary input: ensure the primary signal arg
                // points at the LEFT buffer of the stereo pair — opcode reads
                // inputs[0]+1 for R when STEREO_INPUT is set.
                if (primary_stereo) {
                    expanded_args[static_cast<std::size_t>(expansion_arg_idx)] =
                        expansion_buffers[0];
                }

                // Fill in any remaining defaults with control-rate constants
                for (std::size_t j = expanded_args.size(); j < n_params; ++j) {
                    if (builtin->has_default(j)) {
                        std::uint16_t default_buf = buffers_.allocate();
                        if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            if (pushed_path) pop_path();
                            return TypedValue::error_val();
                        }
                        cedar::Instruction push_inst{};
                        push_inst.opcode = cedar::Opcode::PUSH_CONST;
                        push_inst.out_buffer = default_buf;
                        push_inst.inputs[0] = 0xFFFF;
                        push_inst.inputs[1] = 0xFFFF;
                        push_inst.inputs[2] = 0xFFFF;
                        push_inst.inputs[3] = 0xFFFF;
                        encode_const_value(push_inst, builtin->get_default(j));
                        emit(push_inst);
                        expanded_args.push_back(default_buf);
                    }
                }

                // Allocate adjacent L/R output pair (BufferAllocator is linear
                // so two back-to-back allocations are guaranteed adjacent).
                std::uint16_t out_left = buffers_.allocate();
                std::uint16_t out_right = buffers_.allocate();
                if (out_left == BufferAllocator::BUFFER_UNUSED ||
                    out_right == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }
                if (out_right != out_left + 1) {
                    error("E166", "Internal error: stereo buffer allocation not adjacent",
                          n.location);
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }

                cedar::Instruction inst{};
                inst.opcode = builtin->opcode;
                inst.out_buffer = out_left;
                inst.inputs[0] = expanded_args.size() > 0 ? expanded_args[0] : 0xFFFF;
                inst.inputs[1] = expanded_args.size() > 1 ? expanded_args[1] : 0xFFFF;
                inst.inputs[2] = expanded_args.size() > 2 ? expanded_args[2] : 0xFFFF;
                inst.inputs[3] = expanded_args.size() > 3 ? expanded_args[3] : 0xFFFF;
                inst.inputs[4] = expanded_args.size() > 4 ? expanded_args[4] : 0xFFFF;
                inst.rate = builtin->inst_rate;
                inst.flags = static_cast<std::uint16_t>(
                    cedar::InstructionFlag::STEREO_OUTPUT |
                    (primary_stereo ? cedar::InstructionFlag::STEREO_INPUT : 0u));

                // FM detection on stereo-native oscillators (currently no
                // stereo-native oscillators exist, but keep the path uniform).
                if (is_upgradeable_oscillator(inst.opcode) && !expanded_args.empty()) {
                    if (is_fm_modulated(expanded_args[0])) {
                        inst.opcode = upgrade_for_fm(inst.opcode);
                    }
                }

                // Stereo-native state ID uses plain fnv1a(semantic_path) — no
                // /L suffix, no XOR. Per-channel fields live inside one state
                // struct (see DattorroState predelay_buffer[2], etc.).
                inst.state_id = compute_state_id();
                emit_extended_params_init(inst.state_id, *builtin, arg_buffers);
                emit(inst);

                if (pushed_path) pop_path();

                register_stereo(node, out_left, out_right);
                return cache_and_return(node,
                    TypedValue::stereo_signal(out_left, out_right));
            }

            if (is_stereo_expansion) {
                current_source_loc_ = call_loc;

                std::size_t n_params = builtin->total_params();
                auto expanded_args = arg_buffers;

                // Make sure the primary signal arg points at the LEFT buffer
                // of the stereo pair — the VM computes R as left+1 at dispatch.
                expanded_args[static_cast<std::size_t>(expansion_arg_idx)] =
                    expansion_buffers[0];

                // Fill in any remaining defaults with control-rate constants
                for (std::size_t j = expanded_args.size(); j < n_params; ++j) {
                    if (builtin->has_default(j)) {
                        std::uint16_t default_buf = buffers_.allocate();
                        if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            if (pushed_path) pop_path();
                            return TypedValue::error_val();
                        }
                        cedar::Instruction push_inst{};
                        push_inst.opcode = cedar::Opcode::PUSH_CONST;
                        push_inst.out_buffer = default_buf;
                        push_inst.inputs[0] = 0xFFFF;
                        push_inst.inputs[1] = 0xFFFF;
                        push_inst.inputs[2] = 0xFFFF;
                        push_inst.inputs[3] = 0xFFFF;
                        encode_const_value(push_inst, builtin->get_default(j));
                        emit(push_inst);
                        expanded_args.push_back(default_buf);
                    }
                }

                // Allocate adjacent L/R output pair (BufferAllocator is linear
                // so two back-to-back allocations are guaranteed adjacent).
                std::uint16_t out_left = buffers_.allocate();
                std::uint16_t out_right = buffers_.allocate();
                if (out_left == BufferAllocator::BUFFER_UNUSED ||
                    out_right == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }
                if (out_right != out_left + 1) {
                    error("E166", "Internal error: stereo buffer allocation not adjacent",
                          n.location);
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }

                cedar::Instruction inst{};
                inst.opcode = builtin->opcode;
                inst.out_buffer = out_left;
                inst.inputs[0] = expanded_args.size() > 0 ? expanded_args[0] : 0xFFFF;
                inst.inputs[1] = expanded_args.size() > 1 ? expanded_args[1] : 0xFFFF;
                inst.inputs[2] = expanded_args.size() > 2 ? expanded_args[2] : 0xFFFF;
                inst.inputs[3] = expanded_args.size() > 3 ? expanded_args[3] : 0xFFFF;
                inst.inputs[4] = expanded_args.size() > 4 ? expanded_args[4] : 0xFFFF;
                inst.rate = builtin->inst_rate;
                inst.flags = cedar::InstructionFlag::STEREO_INPUT;

                // FM detection operates on the L pass's frequency input; the R
                // pass sees the same buffer (VM only shifts inputs[0]).
                if (is_upgradeable_oscillator(inst.opcode) && !expanded_args.empty()) {
                    if (is_fm_modulated(expanded_args[0])) {
                        inst.opcode = upgrade_for_fm(inst.opcode);
                    }
                }

                // State ID uses the /L suffix so the mono path
                // (fnv1a(semantic_path)) stays backward-compatible for
                // hot-swap (PRD §5.4). The VM XORs this with
                // STEREO_STATE_XOR_R for the R-channel pass. Stateless
                // opcodes simply ignore state_id, so the /L path is
                // harmless there.
                push_path("L");
                inst.state_id = compute_state_id();
                pop_path();
                emit_extended_params_init(inst.state_id, *builtin, arg_buffers);
                emit(inst);

                if (pushed_path) pop_path();

                register_stereo(node, out_left, out_right);
                return cache_and_return(node,
                    TypedValue::stereo_signal(out_left, out_right));
            }

            // Chord expansion to N instances (stateful UGens only — per-voice state).
            if (builtin->requires_state && expansion_arg_idx >= 0 && expansion_buffers.size() > 1) {
                    std::vector<TypedValue> result_elements;
                    std::size_t n_params = builtin->total_params();

                    for (std::size_t i = 0; i < expansion_buffers.size(); ++i) {
                        // Push unique path for each expansion
                        push_path("elem" + std::to_string(i));

                        // Create argument buffers with expanded element substituted
                        auto expanded_args = arg_buffers;
                        expanded_args[static_cast<std::size_t>(expansion_arg_idx)] = expansion_buffers[i];

                        // Fill in defaults for this instance
                        for (std::size_t j = expanded_args.size(); j < n_params; ++j) {
                            if (builtin->has_default(j)) {
                                std::uint16_t default_buf = buffers_.allocate();
                                if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                                    error("E101", "Buffer pool exhausted", n.location);
                                    pop_path();
                                    if (pushed_path) pop_path();
                                    return TypedValue::error_val();
                                }
                                cedar::Instruction push_inst{};
                                push_inst.opcode = cedar::Opcode::PUSH_CONST;
                                push_inst.out_buffer = default_buf;
                                push_inst.inputs[0] = 0xFFFF;
                                push_inst.inputs[1] = 0xFFFF;
                                push_inst.inputs[2] = 0xFFFF;
                                push_inst.inputs[3] = 0xFFFF;
                                encode_const_value(push_inst, builtin->get_default(j));
                                emit(push_inst);
                                expanded_args.push_back(default_buf);
                            }
                        }

                        // Allocate output buffer for this instance
                        std::uint16_t inst_out = buffers_.allocate();
                        if (inst_out == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            pop_path();
                            if (pushed_path) pop_path();
                            return TypedValue::error_val();
                        }

                        // Build instruction for this instance
                        cedar::Instruction inst{};
                        inst.opcode = builtin->opcode;
                        inst.out_buffer = inst_out;
                        inst.inputs[0] = expanded_args.size() > 0 ? expanded_args[0] : 0xFFFF;
                        inst.inputs[1] = expanded_args.size() > 1 ? expanded_args[1] : 0xFFFF;
                        inst.inputs[2] = expanded_args.size() > 2 ? expanded_args[2] : 0xFFFF;
                        inst.inputs[3] = expanded_args.size() > 3 ? expanded_args[3] : 0xFFFF;
                        inst.inputs[4] = expanded_args.size() > 4 ? expanded_args[4] : 0xFFFF;
                        inst.rate = builtin->inst_rate;

                        // FM detection for this instance
                        if (is_upgradeable_oscillator(inst.opcode) && !expanded_args.empty()) {
                            if (is_fm_modulated(expanded_args[0])) {
                                inst.opcode = upgrade_for_fm(inst.opcode);
                            }
                        }

                        // Compute state_id with unique path
                        inst.state_id = compute_state_id();
                        emit_extended_params_init(inst.state_id, *builtin, expanded_args);
                        emit(inst);

                        result_elements.push_back(TypedValue::signal(inst_out));
                        pop_path();
                    }

                // Pop the outer stateful path
                if (pushed_path) pop_path();

                // Build array result. Stereo-source expansion is already
                // routed through the STEREO_INPUT path above, so we never
                // hit this branch with is_stereo_expansion==true.
                std::uint16_t first_buf = result_elements[0].buffer;
                auto tv = TypedValue::make_array(std::move(result_elements), first_buf);

                return cache_and_return(node, tv);
            }

            // Restore call location before emitting default parameter instructions
            // (visiting arguments may have changed current_source_loc_)
            current_source_loc_ = call_loc;

            // Fill in missing optional arguments with defaults
            std::size_t total_params = builtin->total_params();
            for (std::size_t i = arg_buffers.size(); i < total_params; ++i) {
                if (builtin->has_default(i)) {
                    // Emit PUSH_CONST for the default value
                    std::uint16_t default_buf = buffers_.allocate();
                    if (default_buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        if (pushed_path) pop_path();
                        return TypedValue::error_val();
                    }

                    cedar::Instruction push_inst{};
                    push_inst.opcode = cedar::Opcode::PUSH_CONST;
                    push_inst.out_buffer = default_buf;
                    push_inst.inputs[0] = 0xFFFF;
                    push_inst.inputs[1] = 0xFFFF;
                    push_inst.inputs[2] = 0xFFFF;
                    push_inst.inputs[3] = 0xFFFF;

                    float default_val = builtin->get_default(i);
                    encode_const_value(push_inst, default_val);
                    emit(push_inst);

                    arg_buffers.push_back(default_buf);
                }
            }

            // Scalar SAMPLE_PLAY (the `sample()` builtin) routes through
            // emit_sample_chain so all SAMPLE_PLAY emission sits behind one
            // helper. Pre-migration, this path emitted a single SAMPLE_PLAY
            // here with inputs[3]/[4] populated from arg_buffers (always
            // BUFFER_UNUSED for the 3-input sample() builtin) and state_id
            // from compute_state_id(). emit_sample_chain Kind::Scalar
            // produces the same instruction; verified by golden test G10.
            if (builtin->opcode == cedar::Opcode::SAMPLE_PLAY) {
                SamplePatternEmitCtx ctx;
                ctx.kind = SamplePatternEmitCtx::Kind::Scalar;
                ctx.trigger_buf = arg_buffers.size() > 0 ? arg_buffers[0] : static_cast<std::uint16_t>(0xFFFF);
                // Caller-supplied pitch buffer (the 2nd arg) — helper skips
                // its own PUSH_CONST 1.0 emission when this is set.
                ctx.pitch_buf   = arg_buffers.size() > 1 ? arg_buffers[1] : BufferAllocator::BUFFER_UNUSED;
                ctx.value_buf   = arg_buffers.size() > 2 ? arg_buffers[2] : static_cast<std::uint16_t>(0xFFFF);
                ctx.velocity_buf = BufferAllocator::BUFFER_UNUSED;  // no MUL on scalar path
                ctx.rate = builtin->inst_rate;
                ctx.loc = n.location;
                if (pushed_path) {
                    ctx.seq_state_id = compute_state_id();
                    pop_path();
                } else {
                    ctx.seq_state_id = 0;
                }
                std::uint16_t out = emit_sample_chain(
                    buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    return TypedValue::error_val();
                }
                TypedValue result = TypedValue::signal(out);
                result.channels = builtin->output_channels;
                if (result.channels == ChannelCount::Stereo) {
                    result.right_buffer = static_cast<std::uint16_t>(out + 1);
                }
                return cache_and_return(node, result);
            }

            // Allocate output buffer
            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                if (pushed_path) pop_path();
                return TypedValue::error_val();
            }

            // Build instruction
            cedar::Instruction inst{};
            inst.opcode = builtin->opcode;
            inst.out_buffer = out;
            inst.inputs[0] = arg_buffers.size() > 0 ? arg_buffers[0] : 0xFFFF;
            inst.inputs[1] = arg_buffers.size() > 1 ? arg_buffers[1] : 0xFFFF;
            inst.inputs[2] = arg_buffers.size() > 2 ? arg_buffers[2] : 0xFFFF;
            inst.inputs[3] = arg_buffers.size() > 3 ? arg_buffers[3] : 0xFFFF;
            inst.inputs[4] = arg_buffers.size() > 4 ? arg_buffers[4] : 0xFFFF;
            inst.rate = builtin->inst_rate;

            // Special handling for ADSR: pack release time (arg 4) into rate field
            // Release time in tenths of seconds (0-255 -> 0-25.5s)
            if (func_name == "adsr" && arg_buffers.size() >= 5) {
                // Find the release argument value from AST to extract literal
                NodeIndex adsr_arg = n.first_child;
                for (std::size_t idx = 0; adsr_arg != NULL_NODE && idx < 5; ++idx) {
                    if (idx == 4) {
                        const Node& arg_node = ast_->arena[adsr_arg];
                        NodeIndex arg_value = (arg_node.type == NodeType::Argument) ?
                                             arg_node.first_child : adsr_arg;
                        if (arg_value != NULL_NODE) {
                            const Node& val_node = ast_->arena[arg_value];
                            if (val_node.type == NodeType::NumberLit) {
                                float release_val = static_cast<float>(val_node.as_number());
                                inst.rate = static_cast<std::uint8_t>(
                                    std::clamp(release_val / 0.1f, 0.0f, 255.0f));
                            }
                        }
                        break;
                    }
                    adsr_arg = ast_->arena[adsr_arg].next_sibling;
                }
            }

            // Special handling for delay time units: rate field encodes unit type
            // 0 = seconds (default), 1 = milliseconds, 2 = samples
            if (func_name == "delay") {
                inst.rate = 0;  // seconds
            } else if (func_name == "delay_ms") {
                inst.rate = 1;  // milliseconds
            } else if (func_name == "delay_smp") {
                inst.rate = 2;  // samples
            }

            // PRD prd-extended-params §6b — phaser's `feedback` and `stages`
            // are now full extended params (ExtendedParams<3>, slots 0/1),
            // along with `lfo_phase` (slot 2). The legacy rate-field bit
            // packing is gone; emit_extended_params_init below handles them.

            // Generate state_id from current path (already pushed if stateful)
            if (pushed_path) {
                inst.state_id = compute_state_id();
                pop_path();
            } else {
                inst.state_id = 0;
            }

            // FM Detection: Automatically upgrade oscillators to 4x when frequency
            // input comes from an audio-rate source (another oscillator, noise, etc.)
            if (is_upgradeable_oscillator(inst.opcode) && !arg_buffers.empty()) {
                std::uint16_t freq_buffer = arg_buffers[0];
                if (is_fm_modulated(freq_buffer)) {
                    inst.opcode = upgrade_for_fm(inst.opcode);
                }
            }

            emit_extended_params_init(inst.state_id, *builtin, arg_buffers);
            emit(inst);
            // Propagate the builtin's declared output channel count (PRD §5.2).
            // For the common mono-in/mono-out case this stays Mono. It is the
            // hook for future stereo-native generators (e.g. `in()`) to emit
            // Stereo results through the generic path; today all stereo-output
            // builtins route through codegen_stereo.cpp, so this only ever
            // takes the Mono branch in the current tree.
            TypedValue result = TypedValue::signal(out);
            result.channels = builtin->output_channels;
            if (result.channels == ChannelCount::Stereo) {
                result.right_buffer = static_cast<std::uint16_t>(out + 1);
            }
            return cache_and_return(node, result);
        }

        case NodeType::BinaryOp: {
            // BinaryOp should have been desugared to Call by parser
            // But handle it anyway in case we get one
            NodeIndex lhs = n.first_child;
            NodeIndex rhs = (lhs != NULL_NODE) ?
                           ast_->arena[lhs].next_sibling : NULL_NODE;

            if (lhs == NULL_NODE || rhs == NULL_NODE) {
                error("E108", "Invalid binary operation", n.location);
                return TypedValue::error_val();
            }

            TypedValue lhs_tv = visit(lhs);
            TypedValue rhs_tv = visit(rhs);

            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }

            // Map BinOp to opcode
            cedar::Opcode opcode;
            switch (n.as_binop()) {
                case BinOp::Add: opcode = cedar::Opcode::ADD; break;
                case BinOp::Sub: opcode = cedar::Opcode::SUB; break;
                case BinOp::Mul: opcode = cedar::Opcode::MUL; break;
                case BinOp::Div: opcode = cedar::Opcode::DIV; break;
                case BinOp::Pow: opcode = cedar::Opcode::POW; break;
                default:
                    error("E109", "Unknown binary operator", n.location);
                    return TypedValue::error_val();
            }

            emit(cedar::Instruction::make_binary(opcode, out, lhs_tv.buffer, rhs_tv.buffer));
            return cache_and_return(node, TypedValue::signal(out));
        }

        case NodeType::Hole: {
            // Holes should have been substituted by the analyzer
            error("E110", "Hole '%' in unexpected context", n.location);
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

void CodeGenerator::emit(const cedar::Instruction& inst) {
    instructions_.push_back(inst);
    source_locations_.push_back(current_source_loc_);
}

std::uint32_t CodeGenerator::compute_state_id() const {
    // Build path string
    std::string path;
    for (size_t i = 0; i < path_stack_.size(); ++i) {
        if (i > 0) path += '/';
        path += path_stack_[i];
    }
    return cedar::fnv1a_hash_runtime(path.data(), path.size());
}

void CodeGenerator::push_path(std::string_view segment) {
    path_stack_.push_back(std::string(segment));
}

void CodeGenerator::pop_path() {
    if (!path_stack_.empty()) {
        path_stack_.pop_back();
    }
}

void CodeGenerator::error(const std::string& code, const std::string& message,
                          SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Error;
    diag.code = code;
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

void CodeGenerator::warn(const std::string& code, const std::string& message,
                         SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Warning;
    diag.code = code;
    diag.message = message;
    diag.filename = filename_;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

// FM Detection: Check if buffer was produced by audio-rate source (recursively traces arithmetic)
bool CodeGenerator::is_fm_modulated(std::uint16_t freq_buffer) const {
    for (const auto& inst : instructions_) {
        if (inst.out_buffer == freq_buffer) {
            // Direct audio-rate producer
            if (is_audio_rate_producer(inst.opcode)) {
                return true;
            }
            // Arithmetic on FM source is still FM
            if (inst.opcode == cedar::Opcode::ADD ||
                inst.opcode == cedar::Opcode::SUB ||
                inst.opcode == cedar::Opcode::MUL ||
                inst.opcode == cedar::Opcode::DIV ||
                inst.opcode == cedar::Opcode::POW) {
                // Check if either input traces back to audio-rate source
                if (inst.inputs[0] != 0xFFFF && is_fm_modulated(inst.inputs[0])) {
                    return true;
                }
                if (inst.inputs[1] != 0xFFFF && is_fm_modulated(inst.inputs[1])) {
                    return true;
                }
            }
            // Found the producer but it's not audio-rate
            break;
        }
    }
    return false;
}

// Multi-buffer compatibility helpers (backed by node_types_)
bool CodeGenerator::is_multi_buffer(NodeIndex node) const {
    auto it = node_types_.find(node);
    if (it != node_types_.end()) {
        // Any array (empty or multi-element) goes through multi-buffer paths.
        // Single-element arrays are unwrapped to Signal upstream, so they
        // never reach this check as Array.
        if (it->second.type == ValueType::Array && it->second.array &&
            it->second.array->elements.size() != 1) {
            return true;
        }
    }
    // Fallback: check if the node's buffer is a stereo left channel
    if (it != node_types_.end() && it->second.buffer != BufferAllocator::BUFFER_UNUSED) {
        return stereo_buffer_pairs_.find(it->second.buffer) != stereo_buffer_pairs_.end();
    }
    return false;
}

std::vector<std::uint16_t> CodeGenerator::get_multi_buffers(NodeIndex node) const {
    auto it = node_types_.find(node);
    if (it != node_types_.end()) {
        if (it->second.type == ValueType::Array && it->second.array) {
            return buffers_of(it->second);
        }
        // Fallback: check buffer-based stereo tracking
        if (it->second.buffer != BufferAllocator::BUFFER_UNUSED) {
            auto pair_it = stereo_buffer_pairs_.find(it->second.buffer);
            if (pair_it != stereo_buffer_pairs_.end()) {
                return {it->second.buffer, pair_it->second};
            }
            return {it->second.buffer};
        }
    }
    return {};
}

// Get the TypedValue for a node (checks cache, then follows symbol table)
const TypedValue* CodeGenerator::get_node_type(NodeIndex node) const {
    auto it = node_types_.find(node);
    if (it != node_types_.end()) {
        return &it->second;
    }

    // If it's an identifier, follow symbol table
    const Node& n = ast_->arena[node];
    if (n.type == NodeType::Identifier) {
        std::string var_name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            var_name = n.as_identifier();
        }
        auto sym = symbols_->lookup(var_name);
        if (sym && sym->typed_value) {
            return &*sym->typed_value;
        }
        if (sym && sym->kind == SymbolKind::Record && sym->record_type) {
            auto rec_it = node_types_.find(sym->record_type->source_node);
            if (rec_it != node_types_.end()) {
                return &rec_it->second;
            }
        }
        if (sym && sym->kind == SymbolKind::Pattern) {
            auto pat_it = node_types_.find(sym->pattern.pattern_node);
            if (pat_it != node_types_.end()) {
                return &pat_it->second;
            }
        }
    }

    return nullptr;
}

// Bind destructured fields from a record/pattern TypedValue into the symbol table.
// Phase 3b: each field may carry a default expression (AST node index) used
// when the source is missing that field. Pure missing-field (no default)
// fires `missing_field_code`.
bool CodeGenerator::bind_destructure_fields(
    const TypedValue& source_tv,
    const std::vector<DestructureField>& fields,
    SourceLocation loc,
    const char* missing_field_code)
{
    auto bind_default_or_error = [&](const DestructureField& field,
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

        // Spread argument: `..expr` — Argument node with spread_source set.
        if (a.type == NodeType::Argument &&
            std::holds_alternative<Node::ArgumentData>(a.data) &&
            a.as_argument().spread_source != NULL_NODE) {

            NodeIndex src_node = a.as_argument().spread_source;
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

// ============================================================================
// Record support implementation
// ============================================================================

TypedValue CodeGenerator::handle_record_literal(NodeIndex node, const Node& n) {
    // Record literals expand to multiple buffers - one per field
    // We track the field->TypedValue mapping in RecordPayload

    std::unordered_map<std::string, TypedValue> field_values;
    std::uint16_t first_buffer = BufferAllocator::BUFFER_UNUSED;

    // Handle spread source: {..base, field: value}
    if (std::holds_alternative<Node::RecordLitData>(n.data)) {
        const auto& rec_data = n.as_record_lit();
        if (rec_data.spread_source != NULL_NODE) {
            // Visit the spread source expression
            TypedValue spread_tv = visit(rec_data.spread_source);

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
                const Node& spread_node = ast_->arena[rec_data.spread_source];
                if (spread_node.type == NodeType::Identifier) {
                    std::string var_name;
                    if (std::holds_alternative<Node::IdentifierData>(spread_node.data)) {
                        var_name = spread_node.as_identifier();
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
                            error("E140", "Spread source is not a record", ast_->arena[rec_data.spread_source].location);
                        }
                    } else {
                        error("E140", "Spread source is not a record", ast_->arena[rec_data.spread_source].location);
                    }
                } else {
                    error("E140", "Spread source is not a record", ast_->arena[rec_data.spread_source].location);
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
            var_name = expr.as_identifier();
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
            var_name = expr.as_identifier();
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

            std::uint16_t out = buffers_.allocate();
            if (out == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::error_val();
            }

            cedar::Instruction inst{};
            inst.opcode = cedar::Opcode::STATE_OP;
            inst.rate = 1;  // load mode
            inst.out_buffer = out;
            inst.inputs[0] = 0xFFFF;
            inst.inputs[1] = 0xFFFF;
            inst.inputs[2] = 0xFFFF;
            inst.inputs[3] = 0xFFFF;
            inst.inputs[4] = 0xFFFF;
            inst.state_id = sub_cell.cell_state_id;
            emit(inst);

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
        new_sym.name_hash = fnv1a_hash(binding_name);
        new_sym.buffer_index = expr_tv.buffer;
        new_sym.typed_value = expr_tv;
        symbols_->define(new_sym);
    } else if (expr_tv.type == ValueType::Array && expr_tv.array) {
        Symbol new_sym;
        new_sym.kind = SymbolKind::Variable;
        new_sym.name = binding_name;
        new_sym.name_hash = fnv1a_hash(binding_name);
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
                var_name = expr.as_identifier();
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
    } else {
        warn("W150", "Unknown directive '$" + dir_name + "'", n.location);
    }

    // Directives don't produce values
    return cache_and_return(node, TypedValue::void_val());
}

} // namespace akkado
