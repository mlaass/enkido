#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"  // Master include for all codegen helpers
#include "akkado/builtins.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/source_map.hpp"
#include "akkado/stdlib.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/const_eval.hpp"
#include "akkado/pattern_eval.hpp"
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

CodeGenerator::CodeGenerator(CompileContext& ctx) : ctx_(&ctx) {}

std::uint16_t BufferAllocator::allocate() {
    // LIFO drain of the free list keeps recently-released buffers hot in
    // cache and minimises perturbation of the high-water mark.
    if (!free_list_.empty()) {
        std::uint16_t idx = free_list_.back();
        free_list_.pop_back();
        return idx;
    }
    // BUFFER_ZERO is reserved as the always-zero scratch slot; never hand it out.
    if (next_ == cedar::BUFFER_ZERO) {
        ++next_;
    }
    if (next_ >= MAX_ALLOCATABLE) {
        return BUFFER_UNUSED;
    }
    return next_++;
}

void BufferAllocator::release(std::uint16_t idx) {
    // Releasing a never-allocated, the sentinel, or BUFFER_ZERO (255) is a
    // no-op — keeps callers free to release defensively without checks.
    if (idx == BUFFER_UNUSED) return;
    if (idx >= MAX_ALLOCATABLE) return;
    if (idx >= next_) return;
    free_list_.push_back(idx);
}

void BufferAllocator::reset_to(std::uint16_t mark) {
    if (mark > next_) return;
    next_ = mark;
    // Drop free-list entries that referred to indices the cursor reset
    // has now reclaimed; otherwise a later allocate() could hand out an
    // index that overlaps with whatever the caller emits past the mark.
    free_list_.erase(
        std::remove_if(free_list_.begin(), free_list_.end(),
                       [mark](std::uint16_t idx) { return idx >= mark; }),
        free_list_.end());
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
                                       const SourceMap* source_map,
                                       bool bypass_master) {
    bypass_master_ = bypass_master;
    ast_ = &ast;
    symbols_ = &symbols;
    sample_registry_ = sample_registry;
    source_map_ = source_map;
    buffers_ = BufferAllocator{};
    instructions_.clear();
    source_locations_.clear();
    subprograms_.clear();
    subprogram_stack_.clear();
    shared_blocks_.clear();
    fn_call_counts_.clear();
    diagnostics_.clear();
    state_inits_.clear();
    pre_resolved_values_.clear();
    required_samples_.clear();
    required_samples_extended_keys_.clear();
    required_samples_extended_.clear();
    scalar_sample_mappings_.clear();
    required_soundfonts_.clear();
    soundfont_aliases_.clear();
    required_midi_sources_.clear();
    required_midi_cc_routes_.clear();
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
    user_function_depth_ = 0;
    param_function_refs_.clear();
    polyphonic_pattern_nodes_.clear();
    current_source_loc_ = {};
    options_ = CompilerOptions{};  // Reset compiler options

    // Start with "main" path
    push_path("main");

    if (!ast.valid()) {
        error("E100", "Invalid AST", {});
        CodeGenResult invalid;
        invalid.diagnostics = std::move(diagnostics_);
        invalid.success = false;
        return invalid;
    }

    // PRD L2: pre-pass — count call sites per callee name so the shared
    // BLOCK_CALL lowering can skip fns called fewer than twice.
    count_fn_calls(ast.root);

    // Visit root (Program node)
    visit(ast.root);

    pop_path();

    // prd-bus-routing Phase 1: append the per-block bus epilogue (sum
    // non-zero buses into bus 0, default soft-clip @ 0.9, forced NaN/clamp
    // safety stage, device store) and prepend the bus-clear prologue.
    // Always emitted — a program with no out() simply processes silence.
    emit_bus_epilogue();

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

    CodeGenResult result;
    // PRD L3: the main program ends here; FOREACH_EVENT subprogram bodies are
    // appended after it and each block's absolute offset is resolved.
    result.main_instruction_count =
        static_cast<std::uint32_t>(instructions_.size());
#ifndef NDEBUG
    // PRD prd-parser-codegen-correctness.md Phase 3 (F2): the source-loc
    // parallel vector must stay locked to instructions_. Every push goes
    // through emit(), so this is now structurally enforced — the assert is
    // a paranoid future-proof.
    assert(instructions_.size() == source_locations_.size() &&
           "F2: source_locations_ desync (main stream)");
    for (const auto& desc : subprograms_) {
        assert(desc.body.size() == desc.body_source_locs.size() &&
               "F2: source_locations_ desync (subprogram body)");
    }
#endif
    for (auto& desc : subprograms_) {
        desc.offset = static_cast<std::uint32_t>(instructions_.size());
        instructions_.insert(instructions_.end(),
                             desc.body.begin(), desc.body.end());
        source_locations_.insert(source_locations_.end(),
                                 desc.body_source_locs.begin(),
                                 desc.body_source_locs.end());
    }
#ifndef NDEBUG
    assert(instructions_.size() == source_locations_.size() &&
           "F2: source_locations_ desync after subprogram concat");
#endif

    result.instructions = std::move(instructions_);
    result.subprograms = std::move(subprograms_);
    result.source_locations = std::move(source_locations_);
    result.diagnostics = std::move(diagnostics_);
    result.state_inits = std::move(state_inits_);
    result.required_samples = std::move(required_samples_vec);
    result.required_samples_extended = std::move(required_samples_extended_);
    result.scalar_sample_mappings = std::move(scalar_sample_mappings_);
    result.required_soundfonts = std::move(required_soundfonts_);
    result.required_midi_sources = std::move(required_midi_sources_);
    result.required_midi_cc_routes = std::move(required_midi_cc_routes_);
    result.required_input_sources = std::move(required_input_sources_);
    result.param_decls = std::move(param_decls_);
    result.viz_decls = std::move(viz_decls_);
    result.builtin_var_overrides = std::move(builtin_var_overrides_);
    result.required_wavetables = std::move(required_wavetables_);
    result.required_uris = std::move(required_uris_);
    result.required_buffers = buffers_.peak_count();
    result.success = success;
    return result;
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

            // DynArray (PRD prd-pattern-event-arrays §5.5): data is already
            // packed in one buffer; the length is a runtime signal. Emit
            // ARRAY_INDEX directly with the runtime len_buffer — wrap mode
            // handles per-sample varying length and the empty-event case.
            if (arr_tv.type == ValueType::DynArray && arr_tv.dyn) {
                TypedValue idx_tv = visit(idx_node);
                std::uint16_t out = buffers_.allocate();
                if (out == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    return TypedValue::error_val();
                }
                cedar::Instruction index_inst{};
                index_inst.opcode = cedar::Opcode::ARRAY_INDEX;
                index_inst.out_buffer = out;
                index_inst.inputs[0] = arr_tv.dyn->data_buffer;
                index_inst.inputs[1] = idx_tv.buffer;
                index_inst.inputs[2] = arr_tv.dyn->len_buffer;
                index_inst.inputs[3] = 0xFFFF;
                index_inst.inputs[4] = 0xFFFF;
                index_inst.rate = 0;  // 0 = wrap mode
                index_inst.state_id = 0;
                emit(index_inst);
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
            std::string name = std::string(ctx_->interner->view(n.as_identifier()));

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

            std::string var_name = std::string(ctx_->interner->view(n.as_identifier()));

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
                return handle_user_function_call(node, n, sym->user_function);
            }

            // Check for FunctionValue (lambda assigned to variable)
            if (sym && sym->kind == SymbolKind::FunctionValue) {
                return handle_function_value_call(node, n, sym->function_ref);
            }

            // Builtin/special-handler spread expansion. PRD-parser-codegen-
            // correctness Phase 1a: the original AST is left untouched.
            // expand_call_arguments returns a flat ExpandedArg list which
            // (after optional reorder for named spread fields) is
            // materialised into the local `spread_call_slots` consumed by
            // the per-arg loop.
            std::vector<ExpandedArg> expanded_args;
            std::optional<std::vector<CallSlot>> spread_call_slots;
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
                    expanded_args = std::move(*expanded_opt);
                    did_spread_swap = true;
                }
            }

            // Dispatch table for special function handlers
            using Handler = TypedValue (CodeGenerator::*)(NodeIndex, const Node&);
            static const std::unordered_map<std::string_view, Handler> special_handlers = {
                {"len",     &CodeGenerator::handle_len_call},
                // Bus routing sinks (prd-bus-routing Phase 1). out() is a
                // pure alias for bus(0, ...); both share handle_bus_call so
                // out(@) and bus(0, @) produce byte-identical bytecode.
                {"out",     &CodeGenerator::handle_bus_call},
                {"bus",     &CodeGenerator::handle_bus_call},
                // Per-bus FX (prd-bus-routing Phase 2). master is a pure
                // alias for mixer(0, ...); both share handle_mixer_call.
                {"mixer",   &CodeGenerator::handle_mixer_call},
                {"master",  &CodeGenerator::handle_mixer_call},
                // Pattern-event chord accessors (pattern-event-arrays PRD).
                // Special-cased by name like len/map (not reserved).
                {"notes",   &CodeGenerator::handle_notes_call},
                {"freqs",   &CodeGenerator::handle_freqs_call},
                // User state cells (Phase 3 of userspace-state PRD). state/get/set
                // are reserved at parser level — no user closure can shadow them.
                {"state",   &CodeGenerator::handle_state_call},
                {"get",     &CodeGenerator::handle_get_call},
                {"set",     &CodeGenerator::handle_set_call},
                {"chord",   &CodeGenerator::handle_chord_call},
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
                // Runtime event-stream transforms (closure builtins).
                // transpose/velocity/dur/bend/aftertouch are now stdlib `fn`s
                // defined in akkado/stdlib/event_transforms.ak — see
                // prd-runtime-event-transforms.md §9 Phase 2b.
                {"event_map",    &CodeGenerator::handle_event_map_call},
                {"event_filter", &CodeGenerator::handle_event_filter_call},
                {"bank",      &CodeGenerator::handle_bank_call},
                {"variant",   &CodeGenerator::handle_variant_call},
                {"transport", &CodeGenerator::handle_transport_call},
                {"tune",      &CodeGenerator::handle_tune_call},
                // Phase 2 PRD time/structure transforms.
                // early/late are stdlib `fn`s in event_transforms.ak.
                {"palindrome", &CodeGenerator::handle_palindrome_call},
                {"compress",   &CodeGenerator::handle_compress_call},
                {"ply",        &CodeGenerator::handle_ply_call},
                {"linger",     &CodeGenerator::handle_linger_call},
                {"zoom",       &CodeGenerator::handle_zoom_call},
                {"segment",    &CodeGenerator::handle_segment_call},
                // swing / swingBy are stdlib `fn`s in event_transforms.ak.
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
                {"sf_voice", &CodeGenerator::handle_sf_voice_call},
                // Runtime MIDI event source (PRD prd-midi-input §4.7)
                {"midi", &CodeGenerator::handle_midi_call},
                // MIDI CC / PB / AT → param() route (PRD prd-midi-input §4.8)
                {"midi_cc", &CodeGenerator::handle_midi_cc_call},
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
                // Forward control flow (PRD prd-runtime-functions-control-flow L1)
                {"when",   &CodeGenerator::handle_when_call},
                // Higher-order DSL (PRD prd-runtime-functions-control-flow L3)
                {"each_voice", &CodeGenerator::handle_each_voice_call},
                {"each",       &CodeGenerator::handle_each_call},
            };

            auto handler_it = special_handlers.find(func_name);
            if (handler_it != special_handlers.end()) {
                TypedValue tv = (this->*(handler_it->second))(node, n);
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

            // Spread-expanded builtin calls reach codegen with named Argument
            // nodes still in the chain — the analyzer defers reordering for
            // spread calls (a `..record` source is only known after value
            // evaluation). Map those field names onto the builtin's parameter
            // slots now, so `chorus(@, .., ..{dry: 1, wet: 0.5})` binds dry/wet
            // (incl. extended-param slots) by name instead of being consumed
            // positionally and silently misbinding.
            if (did_spread_swap &&
                !reorder_spread_named_args(*builtin, func_name, expanded_args,
                                           n.location)) {
                return TypedValue::error_val();
            }

            // Materialise the (possibly reordered) ExpandedArg vector into
            // CallSlots for the per-arg loop. Side-table entries are keyed
            // by (call_node, slot_index); the buffer was already allocated
            // by the spread source's earlier visit.
            if (did_spread_swap) {
                std::vector<CallSlot> slots;
                slots.reserve(expanded_args.size());
                for (std::size_t i = 0; i < expanded_args.size(); ++i) {
                    auto& ea = expanded_args[i];
                    CallSlot s;
                    s.loc = ea.loc;
                    if (ea.is_underscore) {
                        s.kind = CallSlot::Kind::Underscore;
                        s.node = NULL_NODE;
                    } else if (ea.resolved.has_value()) {
                        s.kind = CallSlot::Kind::Resolved;
                        s.node = NULL_NODE;
                        pre_resolved_values_[{node, i}] = *ea.resolved;
                    } else {
                        s.kind = CallSlot::Kind::AstNode;
                        // Unwrap Argument if present so downstream consumers
                        // see the value node directly (matches chain-mode).
                        NodeIndex av = ea.source_node;
                        if (av != NULL_NODE) {
                            const Node& sn = ast_->arena[av];
                            if (sn.type == NodeType::Argument) {
                                av = sn.first_child;
                            }
                        }
                        s.node = av;
                    }
                    slots.push_back(std::move(s));
                }
                spread_call_slots = std::move(slots);
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

            // Visit arguments (dependencies must be satisfied).
            //
            // Iteration source: when spread expansion ran, `spread_call_slots`
            // holds the canonical slot-ordered argument list (Resolved,
            // Underscore, or AstNode). Otherwise we synthesise an equivalent
            // CallSlot vector by walking `n.first_child` once — keeping the
            // rest of the loop body single-shape.
            std::vector<std::uint16_t> arg_buffers;
            std::vector<CallSlot> chain_slots;
            const std::vector<CallSlot>* effective_slots = nullptr;
            if (spread_call_slots.has_value()) {
                effective_slots = &*spread_call_slots;
            } else {
                for (NodeIndex it_arg = n.first_child; it_arg != NULL_NODE;
                     it_arg = ast_->arena[it_arg].next_sibling) {
                    NodeIndex arg_value = it_arg;
                    const Node& arg_node = ast_->arena[it_arg];
                    if (arg_node.type == NodeType::Argument) {
                        arg_value = arg_node.first_child;
                    }
                    const Node& val_node = ast_->arena[arg_value];
                    bool is_placeholder =
                        (val_node.type == NodeType::Identifier &&
                         std::holds_alternative<Node::IdentifierData>(val_node.data) &&
                         ctx_->interner->view(val_node.as_identifier()) == "_");
                    CallSlot s;
                    s.loc = val_node.location;
                    if (is_placeholder) {
                        s.kind = CallSlot::Kind::Underscore;
                        s.node = NULL_NODE;
                    } else {
                        s.kind = CallSlot::Kind::AstNode;
                        s.node = arg_value;
                    }
                    chain_slots.push_back(std::move(s));
                }
                effective_slots = &chain_slots;
            }

            for (std::size_t arg_idx = 0; arg_idx < effective_slots->size();
                 ++arg_idx) {
                const CallSlot& slot = (*effective_slots)[arg_idx];

                if (slot.kind == CallSlot::Kind::Underscore) {
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
                              "' — no default value", slot.loc);
                        arg_buffers.push_back(0);
                    }
                    continue;
                }

                TypedValue arg_tv;
                SourceLocation diag_loc;

                if (slot.kind == CallSlot::Kind::Resolved) {
                    auto pr_it = pre_resolved_values_.find({node, arg_idx});
                    arg_tv = (pr_it != pre_resolved_values_.end())
                                 ? pr_it->second
                                 : TypedValue::error_val();
                    diag_loc = slot.loc;
                } else {
                    // CallSlot::Kind::AstNode
                    NodeIndex arg_value = slot.node;
                    arg_tv = visit(arg_value);
                    diag_loc = ast_->arena[arg_value].location;

                    // sample("name"[, ...]) form: parse the sample-name string,
                    // register it for runtime loading, and emit a PUSH_CONST
                    // placeholder whose state_id immediate is patched to the
                    // bank-assigned sample ID after samples are loaded. The
                    // numeric `sample(t, p, <int>)` form falls through.
                    const Node& val_node = ast_->arena[arg_value];
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
                          diag_loc);
                }

                // PRD prd-pattern-event-arrays §4.5/§5.6: a DynArray
                // (notes(e)/freqs(e)) has a runtime-varying length, so it
                // cannot auto-fan-out across a builtin's fixed arity the
                // way a static array does. Reject it with a directive
                // pointing at poly() for runtime polyphony.
                if (arg_tv.type == ValueType::DynArray) {
                    error("E181",
                          func_name + "() cannot auto-expand over a dynamic "
                          "array (chord size varies per pattern event). "
                          "Wrap with poly() for runtime polyphony:\n"
                          "  e |> poly(@, (f, g, v) -> osc(\"sin\", f) * "
                          "ar(g, 0.01, 0.3) * v)",
                          diag_loc);
                }

                arg_buffers.push_back(arg_tv.buffer);

                // Type check against annotation (non-fatal — continue for max error reporting).
                // DynArray already reported the dedicated E181 above; skip
                // the generic type-mismatch to avoid a duplicate diagnostic.
                if (arg_idx < MAX_BUILTIN_PARAMS &&
                    builtin->param_types[arg_idx] != ParamValueType::Any &&
                    !arg_tv.error && arg_tv.type != ValueType::Void &&
                    arg_tv.type != ValueType::DynArray) {
                    if (!type_compatible(arg_tv.type, builtin->param_types[arg_idx])) {
                        error("E160", func_name + "() argument '" +
                              std::string(builtin->param_names[arg_idx]) + "' expects " +
                              param_value_type_name(builtin->param_types[arg_idx]) +
                              ", got " + value_type_name(arg_tv.type),
                              diag_loc);
                    }
                }
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
            // For builtins that are not stereo_native, a stereo signal in a
            // Mono slot (or vice versa) is a compile error E186. Special-handler
            // builtins (stereo/mono/left/right/pan/width/ms_encode/ms_decode/
            // pingpong) never reach this path — they enforce their own
            // signatures with E181–E184. `out()` is handled above via E185 and
            // the single-arg expansion branch.
            //
            // Stereo-native opcodes (prd-stereo-native-opcodes) skip this check
            // because they auto-escalate mono → stereo at the boundary (mono
            // input is broadcast inside the opcode body). As of Phase 5 every
            // audio-signal opcode is stereo-native; auto-lift is retired.
            //
            // Gated on !did_spread_swap: with spread expansion the original
            // first_child chain may contain a spread Argument whose
            // first_child is NULL_NODE (the spread source lives in
            // spread_source, not as a child) — re-visiting that would
            // segfault. Pre-Phase-1a the synthesized chain didn't have this
            // shape; we preserve byte-identical behaviour by skipping the
            // re-walk. Per-arg type checks for the spread case already ran
            // inside the per-arg loop using the cached TypedValues.
            if (func_name != "out" && !builtin->stereo_native && !did_spread_swap) {
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
            // Stereo input is consumed by the stereo-native emission path
            // below (single dispatch, STEREO_INPUT flag). Chord expansion-to-N
            // applies to stateful UGens (per-voice state).
            //
            // Gated on !did_spread_swap: a spread-expanded call has already
            // unpacked its array/record source into individual per-slot
            // values via expand_call_arguments. Re-running multi-buffer
            // fan-out here would double-expand. Pre-Phase-1a, the synthesized
            // PreResolved children weren't multi-buffer sources so the loop
            // silently no-op'd; post-Phase-1a we read the original chain,
            // whose spread-source array IS multi-buffer, so we must skip
            // explicitly. See prd-parser-codegen-correctness.md §4 Phase 1a.
            int expansion_arg_idx = -1;
            std::vector<std::uint16_t> expansion_buffers;
            std::vector<NodeIndex> arg_nodes;

            if (!arg_buffers.empty() && !did_spread_swap) {
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
                        std::string name = std::string(ctx_->interner->view(arg_n.as_identifier()));
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

            }

            // PRD prd-stereo-native-opcodes Phase 1: declarative stereo-native
            // emission. Builtins that opt in produce a stereo output pair in a
            // single dispatch, regardless of input channel count (mono auto-
            // escalates to L=R inside the opcode body). Stereo primary input
            // adds STEREO_INPUT to the flag combo so the opcode reads
            // inputs[0]+1 for R. Chord/polyphonic expansion onto a stereo-
            // native opcode is rejected — user must wrap explicitly in poly().
            // Array expansion on a non-signal (control) slot of a stereo-native
            // opcode — e.g. `lp(sig, [500, 1000, 2000])` — falls through to
            // the chord/array expansion loop below. The loop emits N stereo-
            // native instances (one per expanded element); see
            // prd-stereo-native-opcodes §9.10 (pattern/array events on control
            // slots are unaffected by stereo-native processing).
            const bool stereo_native_control_expansion =
                builtin->stereo_native &&
                expansion_arg_idx > 0 &&
                expansion_buffers.size() > 1;

            if (builtin->stereo_native && !stereo_native_control_expansion) {
                bool primary_stereo =
                    expansion_arg_idx == 0 &&
                    expansion_buffers.size() == 2 &&
                    is_stereo(arg_nodes[0]);

                // Output width: Stereo declares "always emit a pair";
                // Match declares "follow the primary signal input" — when
                // the primary is mono, emit a single buffer so the result
                // slots into downstream mono parameter slots without E186.
                bool emit_stereo;
                switch (builtin->output_channels) {
                    case ChannelCount::Match:
                        emit_stereo = primary_stereo;
                        break;
                    case ChannelCount::Mono:
                        emit_stereo = false;
                        break;
                    case ChannelCount::Stereo:
                    default:
                        emit_stereo = true;
                        break;
                }

                // Chord/poly expansion onto the PRIMARY SIGNAL slot of a
                // stereo-native opcode = E187. Each voice would carry its own
                // stereo pair, ambiguous without poly(). Stereo input (size==2
                // + is_stereo on slot 0) is fine; >2 voices or 2 non-stereo
                // voices indicate a chord that needs poly().
                bool is_chord_expansion_on_signal =
                    expansion_arg_idx == 0 &&
                    expansion_buffers.size() > 1 &&
                    !primary_stereo;
                if (is_chord_expansion_on_signal) {
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

                // Allocate output buffer(s): adjacent L/R pair when emitting
                // stereo, otherwise a single buffer (Match+mono input path).
                std::uint16_t out_left = buffers_.allocate();
                if (out_left == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    if (pushed_path) pop_path();
                    return TypedValue::error_val();
                }
                std::uint16_t out_right = 0xFFFF;
                if (emit_stereo) {
                    out_right = buffers_.allocate();
                    if (out_right == BufferAllocator::BUFFER_UNUSED) {
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
                    (emit_stereo ? cedar::InstructionFlag::STEREO_OUTPUT : 0u) |
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

                if (emit_stereo) {
                    register_stereo(node, out_left, out_right);
                    return cache_and_return(node,
                        TypedValue::stereo_signal(out_left, out_right));
                }
                return cache_and_return(node, TypedValue::signal(out_left));
            }

            // Chord expansion to N instances (stateful UGens only — per-voice state).
            // For stereo-native builtins, each instance allocates an adjacent L/R
            // output pair and sets STEREO_OUTPUT (prd-stereo-native-opcodes §9.10).
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

                        // Output width: Stereo declares "always emit a pair";
                        // Match declares "follow primary signal input", which
                        // is always single-buffer on this branch (expansion
                        // is on a control slot, slot 0 stays mono).
                        bool emit_stereo;
                        if (builtin->stereo_native) {
                            switch (builtin->output_channels) {
                                case ChannelCount::Match:
                                case ChannelCount::Mono:
                                    emit_stereo = false;
                                    break;
                                case ChannelCount::Stereo:
                                default:
                                    emit_stereo = true;
                                    break;
                            }
                        } else {
                            emit_stereo = false;
                        }

                        // Allocate output buffer(s): adjacent L/R pair when
                        // emitting stereo, single buffer otherwise.
                        std::uint16_t inst_out = buffers_.allocate();
                        std::uint16_t inst_out_r = 0xFFFF;
                        if (inst_out == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            pop_path();
                            if (pushed_path) pop_path();
                            return TypedValue::error_val();
                        }
                        if (emit_stereo) {
                            inst_out_r = buffers_.allocate();
                            if (inst_out_r == BufferAllocator::BUFFER_UNUSED) {
                                error("E101", "Buffer pool exhausted", n.location);
                                pop_path();
                                if (pushed_path) pop_path();
                                return TypedValue::error_val();
                            }
                            if (inst_out_r != inst_out + 1) {
                                error("E166", "Internal error: stereo buffer allocation not adjacent",
                                      n.location);
                                pop_path();
                                if (pushed_path) pop_path();
                                return TypedValue::error_val();
                            }
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
                        if (emit_stereo) {
                            inst.flags = static_cast<std::uint16_t>(
                                cedar::InstructionFlag::STEREO_OUTPUT);
                            // (Stereo input through array expansion on a control
                            // slot is not currently expressible — the expansion
                            // arg occupies slot >0; slot 0 stays single-buffer.)
                        }

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

                        if (emit_stereo) {
                            register_stereo(node, inst_out, inst_out_r);
                            result_elements.push_back(
                                TypedValue::stereo_signal(inst_out, inst_out_r));
                        } else {
                            result_elements.push_back(TypedValue::signal(inst_out));
                        }
                        pop_path();
                    }

                // Pop the outer stateful path
                if (pushed_path) pop_path();

                // Build array result. Stereo-source primary input is handled
                // by the stereo-native emission path above (single dispatch),
                // so this branch only ever sees chord/array expansion.
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
                // Match on a non-stereo-native builtin falls back to Mono
                // (Match is only meaningful on the stereo-native path).
                result.channels =
                    (builtin->output_channels == ChannelCount::Stereo)
                        ? ChannelCount::Stereo
                        : ChannelCount::Mono;
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
            // Match only resolves on the stereo-native path; if we land
            // here with Match, treat as Mono (no Stereo pair allocated).
            result.channels =
                (builtin->output_channels == ChannelCount::Stereo)
                    ? ChannelCount::Stereo
                    : ChannelCount::Mono;
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

void CodeGenerator::emit(const cedar::Instruction& inst) {
    // PRD L3: while a FOREACH_EVENT subprogram body is open, instructions land
    // in the body descriptor instead of the main stream.
    if (!subprogram_stack_.empty()) {
        SubprogramDesc& desc = subprograms_[subprogram_stack_.back()];
        desc.body.push_back(inst);
        desc.body_source_locs.push_back(current_source_loc_);
        return;
    }
    instructions_.push_back(inst);
    source_locations_.push_back(current_source_loc_);
}

std::vector<cedar::Instruction>& CodeGenerator::emit_stream() {
    // Route free-function emit helpers to the same place emit() writes: the
    // open subprogram body, or the main stream when none is open.
    if (!subprogram_stack_.empty()) {
        return subprograms_[subprogram_stack_.back()].body;
    }
    return instructions_;
}

std::vector<SourceLocation>& CodeGenerator::loc_stream() {
    // Parallel to emit_stream(): the source-location vector for whatever
    // instruction stream is currently active. F2 callers that need to
    // insert at a non-tail position touch BOTH streams in lock-step.
    if (!subprogram_stack_.empty()) {
        return subprograms_[subprogram_stack_.back()].body_source_locs;
    }
    return source_locations_;
}

// PRD prd-parser-codegen-correctness.md Phase 3 (F2): the four method
// helpers below route every PUSH_CONST / MTOF emission through emit(), so
// `source_locations_` cannot fall out of sync with `instructions_`. The
// previous free-function helpers (codegen::emit_push_const, ::emit_zero,
// ::emit_midi_to_freq, ::finalize_array_result) pushed into the instruction
// vector directly and required callers to remember an adjacent
// `source_locations_.push_back(...)` compensation — most callers didn't,
// silently shifting the parallel arrays from that point on.

std::uint16_t CodeGenerator::emit_push_const(float value) {
    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::PUSH_CONST;
    inst.out_buffer = out;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    codegen::encode_const_value(inst, value);
    emit(inst);
    return out;
}

std::uint16_t CodeGenerator::emit_zero() {
    return emit_push_const(0.0f);
}

std::uint16_t CodeGenerator::emit_midi_to_freq(float midi_note) {
    std::uint16_t midi_buf = emit_push_const(midi_note);
    if (midi_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    std::uint16_t freq_buf = buffers_.allocate();
    if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    cedar::Instruction mtof_inst{};
    mtof_inst.opcode = cedar::Opcode::MTOF;
    mtof_inst.out_buffer = freq_buf;
    mtof_inst.inputs[0] = midi_buf;
    mtof_inst.inputs[1] = 0xFFFF;
    mtof_inst.inputs[2] = 0xFFFF;
    mtof_inst.inputs[3] = 0xFFFF;
    mtof_inst.state_id = 0;
    emit(mtof_inst);
    return freq_buf;
}

TypedValue CodeGenerator::finalize_array_result(
    NodeIndex node, std::vector<std::uint16_t> result_buffers) {
    if (result_buffers.empty()) {
        std::uint16_t zero = emit_zero();
        auto tv = TypedValue::signal(zero);
        node_types_[node] = tv;
        return tv;
    }
    if (result_buffers.size() == 1) {
        auto tv = TypedValue::signal(result_buffers[0]);
        node_types_[node] = tv;
        return tv;
    }
    std::uint16_t first_buf = result_buffers[0];
    std::vector<TypedValue> elements;
    elements.reserve(result_buffers.size());
    for (auto buf : result_buffers) {
        elements.push_back(TypedValue::signal(buf));
    }
    auto tv = TypedValue::make_array(std::move(elements), first_buf);
    node_types_[node] = tv;
    return tv;
}

std::uint32_t CodeGenerator::begin_subprogram() {
    const std::uint32_t block_id =
        static_cast<std::uint32_t>(subprograms_.size());
    SubprogramDesc desc;
    desc.block_id = block_id;
    subprograms_.push_back(std::move(desc));
    subprogram_stack_.push_back(block_id);
    return block_id;
}

void CodeGenerator::end_subprogram(std::uint32_t block_id,
                                   std::uint16_t frame_slot_count,
                                   std::uint8_t output_count) {
    if (block_id < subprograms_.size()) {
        subprograms_[block_id].frame_slot_count = frame_slot_count;
        subprograms_[block_id].output_count = output_count;
    }
    if (!subprogram_stack_.empty() && subprogram_stack_.back() == block_id) {
        subprogram_stack_.pop_back();
    }
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

// ===========================================================================
// Bus routing — prd-bus-routing Phase 1
// ===========================================================================
// handle_bus_call serves both out() and bus(). out(...) is a pure alias for
// bus(0, ...); routing both through one handler guarantees the byte-identical
// bytecode the PRD requires. It emits an OUTPUT instruction whose out_buffer
// is a bus *placeholder* (bus_placeholder(N)); emit_bus_epilogue later
// allocates the real per-bus scratch buffers and rewrites the placeholders.
TypedValue CodeGenerator::handle_bus_call(NodeIndex node, const Node& n) {
    const std::string func_name = std::string(ctx_->interner->view(n.as_identifier()));  // "out" or "bus"
    const SourceLocation call_loc = n.location;
    current_source_loc_ = call_loc;

    // Gather argument child nodes.
    std::vector<NodeIndex> args;
    for (NodeIndex c = n.first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        args.push_back(c);
    }

    // Resolve the bus index. out(...) targets bus 0; bus(N, ...) takes a
    // compile-time non-negative integer literal as its first argument.
    int bus_index = 0;
    std::size_t sig_start = 0;
    if (func_name == "bus") {
        if (args.empty()) {
            error("E260", "bus() requires a bus index and a signal argument",
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        const Node& idx_arg = ast_->arena[args[0]];
        NodeIndex idx_val = (idx_arg.type == NodeType::Argument)
                                ? idx_arg.first_child : args[0];
        bool ok = idx_val != NULL_NODE &&
                  ast_->arena[idx_val].type == NodeType::NumberLit;
        double raw = ok ? ast_->arena[idx_val].as_number() : 0.0;
        if (ok && (raw < 0.0 || std::floor(raw) != raw)) ok = false;
        if (!ok) {
            error("E260",
                  "bus() index must be a compile-time non-negative integer "
                  "literal", call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        if (static_cast<int>(raw) >= MAX_BUS_INDEX) {
            error("E260",
                  "bus() index must be below " + std::to_string(MAX_BUS_INDEX),
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        bus_index = static_cast<int>(raw);
        sig_start = 1;
    }

    const std::size_t sig_count = args.size() - sig_start;
    if (sig_count < 1 || sig_count > 2) {
        error("E260", func_name + "() expects 1 or 2 signal arguments",
              call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }

    // Resolve a signal argument to its left/right buffers. Mirrors the
    // mono-broadcast / stereo-split handling of the legacy out() path.
    struct Chan { std::uint16_t left; std::uint16_t right; bool stereo; };
    auto resolve = [&](NodeIndex child) -> Chan {
        const Node& an = ast_->arena[child];
        NodeIndex val = (an.type == NodeType::Argument) ? an.first_child : child;
        TypedValue tv = visit(val);
        // Argument type check — mirrors the generic builtin path (E160).
        // out()/bus() signal slots are declared Signal; String/Array/etc.
        // are rejected. Number and Pattern auto-promote (type_compatible).
        if (val != NULL_NODE && !tv.error && tv.type != ValueType::Void &&
            tv.type != ValueType::DynArray &&
            !type_compatible(tv.type, ParamValueType::Signal)) {
            error("E160",
                  func_name + "() argument expects " +
                  param_value_type_name(ParamValueType::Signal) +
                  ", got " + value_type_name(tv.type),
                  ast_->arena[val].location);
        }
        std::uint16_t buf = tv.buffer;
        bool node_stereo = (val != NULL_NODE) && is_stereo(val);
        bool buf_stereo = (buf != BufferAllocator::BUFFER_UNUSED) &&
                          is_stereo_buffer(buf);
        if (node_stereo || buf_stereo) {
            StereoBuffers sb = node_stereo ? get_stereo_buffers(val)
                                           : get_stereo_buffers_by_buffer(buf);
            return {sb.left, sb.right, true};
        }
        return {buf, buf, false};  // mono: broadcast L = R
    };

    // Normal: write to a bus placeholder (emit_bus_epilogue allocates the
    // real buffer and runs the master chain). bypass_master_: write straight
    // to the device sink, raw — no bus, no epilogue (test-only).
    const std::uint16_t dst =
        bypass_master_ ? std::uint16_t{0xFFFF} : bus_placeholder(bus_index);
    const std::uint16_t out_flags =
        bypass_master_ ? std::uint16_t{0}
                       : std::uint16_t{cedar::InstructionFlag::BUS_WRITE};
    auto emit_output = [&](std::uint16_t l, std::uint16_t r) {
        cedar::Instruction o{};
        o.opcode = cedar::Opcode::OUTPUT;
        o.rate = 0;
        o.out_buffer = dst;
        o.inputs[0] = l;
        o.inputs[1] = r;
        o.inputs[2] = 0xFFFF;
        o.inputs[3] = 0xFFFF;
        o.inputs[4] = 0xFFFF;
        o.flags = out_flags;
        o.state_id = 0;
        emit(o);
    };

    if (sig_count == 1) {
        Chan c = resolve(args[sig_start]);
        if (!c.stereo && func_name == "bus") {
            warn("W202",
                 "bus(): mono signal auto-broadcast to both channels (L = R)",
                 call_loc);
        }
        current_source_loc_ = call_loc;
        emit_output(c.left, c.right);
    } else {
        Chan a = resolve(args[sig_start]);
        Chan b = resolve(args[sig_start + 1]);
        current_source_loc_ = call_loc;
        if (a.stereo || b.stereo) {
            warn("W185",
                 func_name + "() with mixed channel shapes — auto-escalating: "
                 "mono args broadcast to both buses, stereo args drive both "
                 "buses; contributions sum.", call_loc);
            emit_output(a.left, a.right);
            emit_output(b.left, b.right);
        } else {
            // Explicit L / R: a single OUTPUT with distinct channels.
            emit_output(a.left, b.left);
        }
    }

    return cache_and_return(node, TypedValue::void_val());
}

// handle_mixer_call serves both mixer(N, closure) and master(closure).
// master(c) is a pure alias for mixer(0, c). It validates the call, records a
// MixerCall, and emits NOTHING at the call site — emit_bus_epilogue inlines
// the closure body into the per-bus epilogue (prd-bus-routing Phase 2 §3.3).
TypedValue CodeGenerator::handle_mixer_call(NodeIndex node, const Node& n) {
    const std::string func_name = std::string(ctx_->interner->view(n.as_identifier()));  // "mixer" or "master"
    const SourceLocation call_loc = n.location;
    current_source_loc_ = call_loc;

    // Gather argument child nodes.
    std::vector<NodeIndex> args;
    for (NodeIndex c = n.first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        args.push_back(c);
    }

    // Resolve the bus index. master(...) targets bus 0; mixer(N, ...) takes a
    // compile-time non-negative integer literal as its first argument
    // (validation mirrors handle_bus_call exactly).
    int bus_index = 0;
    NodeIndex closure_arg = NULL_NODE;
    if (func_name == "mixer") {
        if (args.size() < 2) {
            error("E260",
                  "mixer() requires a bus index and a closure argument",
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        const Node& idx_arg = ast_->arena[args[0]];
        NodeIndex idx_val = (idx_arg.type == NodeType::Argument)
                                ? idx_arg.first_child : args[0];
        bool ok = idx_val != NULL_NODE &&
                  ast_->arena[idx_val].type == NodeType::NumberLit;
        double raw = ok ? ast_->arena[idx_val].as_number() : 0.0;
        if (ok && (raw < 0.0 || std::floor(raw) != raw)) ok = false;
        if (!ok) {
            error("E260",
                  "mixer() index must be a compile-time non-negative integer "
                  "literal", call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        if (static_cast<int>(raw) >= MAX_BUS_INDEX) {
            error("E260",
                  "mixer() index must be below " + std::to_string(MAX_BUS_INDEX),
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        bus_index = static_cast<int>(raw);
        closure_arg = args[1];
    } else {  // master
        if (args.empty()) {
            error("E260", "master() requires a closure argument", call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        closure_arg = args[0];
    }

    // Unwrap an Argument wrapper, then require an inline closure literal.
    if (ast_->arena[closure_arg].type == NodeType::Argument) {
        closure_arg = ast_->arena[closure_arg].first_child;
    }
    if (closure_arg == NULL_NODE ||
        ast_->arena[closure_arg].type != NodeType::Closure) {
        error("E260",
              func_name + "() expects an inline closure argument "
              "(e.g. (s) -> s |> ...)", call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }

    // Resolve params + arity via the shared function-arg resolver.
    auto ref = resolve_function_arg(closure_arg);
    if (!ref || ref->is_user_function) {
        error("E260", func_name + "() closure could not be resolved",
              call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }
    // The closure must take exactly 1 (stereo) or 2 (left, right) plain
    // signal parameters — no destructure, no rest param.
    bool arity_ok = (ref->params.size() == 1 || ref->params.size() == 2);
    for (const auto& p : ref->params) {
        if (p.is_destructure || p.is_rest) arity_ok = false;
    }
    if (!arity_ok) {
        error("E262",
              func_name + "() closure must take exactly 1 (stereo) or 2 "
              "(left, right) plain parameters", call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }

    // Locate the closure body (last child of the Closure node) and reject a
    // sink call inside it (out/bus/mixer/master) — rules out routing cycles.
    NodeIndex body = NULL_NODE;
    for (NodeIndex c = ast_->arena[closure_arg].first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        body = c;
    }
    scan_closure_for_sinks(body);

    MixerCall mc;
    mc.bus_index = bus_index;
    mc.closure_node = closure_arg;
    mc.arity = static_cast<int>(ref->params.size());
    mc.param_l = ref->params[0].name;
    if (ref->params.size() == 2) mc.param_r = ref->params[1].name;
    mc.call_loc = call_loc;
    mixer_calls_.push_back(std::move(mc));

    return cache_and_return(node, TypedValue::void_val());
}

// scan_closure_for_sinks recursively walks a mixer/master closure body and
// emits E261 for every out/bus/mixer/master call found inside it. (The <>
// diamond operator is Phase 3 — no token exists yet to scan for.)
bool CodeGenerator::scan_closure_for_sinks(NodeIndex body) {
    if (body == NULL_NODE) return false;
    const Node& n = ast_->arena[body];
    bool found = false;
    if (n.type == NodeType::Call) {
        const std::string callee = std::string(ctx_->interner->view(n.as_identifier()));
        if (callee == "out" || callee == "bus" || callee == "mixer" ||
            callee == "master") {
            error("E261",
                  callee + "() is not allowed inside a mixer/master closure "
                  "body", n.location);
            found = true;
        }
    }
    for (NodeIndex c = n.first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        if (scan_closure_for_sinks(c)) found = true;
    }
    return found;
}

// inline_mixer_closure inlines one mixer/master closure body into the bus
// epilogue, processing the bus stereo pair (bus_l, bus_r) in place.
void CodeGenerator::inline_mixer_closure(const MixerCall& mc,
                                         std::uint16_t bus_l,
                                         std::uint16_t bus_r) {
    // Body = last child of the Closure node.
    NodeIndex body = NULL_NODE;
    for (NodeIndex c = ast_->arena[mc.closure_node].first_child;
         c != NULL_NODE; c = ast_->arena[c].next_sibling) {
        body = c;
    }
    if (body == NULL_NODE) return;

    // Stable semantic-ID path keyed on the bus index (not a call counter) so
    // stateful opcodes inside the closure rebind across recompiles
    // (prd-bus-routing §9 hot-swap).
    push_path("mixer#" + std::to_string(mc.bus_index));
    symbols_->push_scope();

    if (mc.arity == 1) {
        // Bind the single param to the bus stereo pair: the L buffer is the
        // symbol; (L, R) recorded in stereo_buffer_pairs_ so stereo-native
        // ops in the body see a stereo input.
        symbols_->define_variable(mc.param_l, bus_l);
        stereo_buffer_pairs_[bus_l] = bus_r;
    } else {
        // Two mono params: left and right channels separately.
        symbols_->define_variable(mc.param_l, bus_l);
        symbols_->define_variable(mc.param_r, bus_r);
    }

    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    TypedValue result = visit(body);

    node_types_ = std::move(saved_node_types);
    symbols_->pop_scope();
    pop_path();

    // Resolve the result's stereo pair (variable/alias TypedValues may not
    // carry the R buffer directly — recover it from the legacy stereo map).
    std::uint16_t rl = result.buffer;
    std::uint16_t rr = result.right_buffer;
    bool stereo = result.is_stereo() || is_stereo_buffer(rl);
    if (stereo && rr == 0xFFFF && rl != BufferAllocator::BUFFER_UNUSED) {
        StereoBuffers sb = get_stereo_buffers_by_buffer(rl);
        rl = sb.left;
        rr = sb.right;
    }
    if (rl == BufferAllocator::BUFFER_UNUSED) {
        // Closure produced no value — leave the bus signal unchanged.
        return;
    }

    // Copy the processed result back into the bus pair in place. op_copy is
    // mono-only, so a stereo result needs two COPYs; a mono result is
    // broadcast L = R with a W204 warning (prd-bus-routing §3.3).
    if (stereo) {
        // The closure parameter is bound directly to bus_l (and bus_r is
        // tracked as its stereo pair), so rl/rr may alias the destination
        // bus buffers. Patterns like `stereo(0, left(sg))` yield
        // rr == bus_l, and `stereo(right(sg), left(sg))` yields a full
        // L/R swap. A naive `bus_l ← rl; bus_r ← rr` clobbers bus_l
        // before bus_r reads it. Reorder or use a temp to break the
        // read-after-write hazard.
        const bool swap = (rl == bus_r && rr == bus_l);
        const bool right_reads_bus_l = (rr == bus_l && rl != bus_r);
        if (swap) {
            std::uint16_t tmp = buffers_.allocate();
            if (tmp == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", mc.call_loc);
                return;
            }
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, tmp,
                                                bus_l));
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_l,
                                                bus_r));
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_r,
                                                tmp));
        } else if (right_reads_bus_l) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_r,
                                                rr));
            if (rl != bus_l) {
                emit(cedar::Instruction::make_unary(cedar::Opcode::COPY,
                                                    bus_l, rl));
            }
        } else {
            if (rl != bus_l) {
                emit(cedar::Instruction::make_unary(cedar::Opcode::COPY,
                                                    bus_l, rl));
            }
            if (rr != bus_r) {
                emit(cedar::Instruction::make_unary(cedar::Opcode::COPY,
                                                    bus_r, rr));
            }
        }
    } else {
        warn("W204",
             "mixer/master closure returned a mono value — auto-broadcast "
             "L = R", mc.call_loc);
        if (rl != bus_l) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_l,
                                                rl));
        }
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_r, rl));
    }
}

// emit_bus_epilogue runs once, after the main DAG is generated. It allocates
// the per-bus scratch buffers, rewrites bus placeholders to real indices,
// appends the per-block epilogue (sum non-zero buses into bus 0, default
// soft-clip @ 0.9, forced NaN/clamp safety stage, device store) and prepends
// the prologue that clears every bus accumulator to silence.
void CodeGenerator::emit_bus_epilogue() {
    // Test-only bypass: out()/bus() already emitted plain device writes.
    if (bypass_master_) return;

    current_source_loc_ = SourceLocation{};

    constexpr std::uint16_t kPlaceLo = 0xFF00;
    constexpr std::uint16_t kPlaceHi = 0xFFFE;  // 0xFFFF stays the device sink
    auto is_placeholder = [](std::uint16_t b) {
        return b >= kPlaceLo && b <= kPlaceHi;
    };

    // 1. Collect every referenced bus index from emitted OUTPUT writers
    //    (main stream + subprogram bodies). Bus 0 always exists.
    std::set<int> indices;
    indices.insert(0);
    std::set<int> writer_indices;   // bus indices with ≥1 out()/bus() writer
    std::size_t writer_count = 0;
    auto scan = [&](const std::vector<cedar::Instruction>& code) {
        for (const auto& inst : code) {
            if (inst.opcode == cedar::Opcode::OUTPUT) {
                ++writer_count;
                if (is_placeholder(inst.out_buffer)) {
                    int idx = inst.out_buffer - kPlaceLo;
                    indices.insert(idx);
                    writer_indices.insert(idx);
                }
            }
        }
    };
    scan(instructions_);
    for (const auto& desc : subprograms_) scan(desc.body);

    // Per-bus FX (Phase 2): a mixer(N,…)/master(…) call references a bus even
    // when no out()/bus() writes to it — allocate that bus too.
    for (const auto& mc : mixer_calls_) indices.insert(mc.bus_index);

    // A program with no out()/bus() writer produces no audio — skip the
    // whole bus epilogue. (prd-bus-routing §5.3 specifies an always-emitted
    // epilogue; emitting it only when a sink exists is functionally
    // identical for every audible program and keeps pure-computation
    // snippets free of a silent master chain.)
    if (writer_count == 0) {
        return;
    }

    // 1b. Per-bus FX: resolve mixer/master overrides — the last call per bus
    //     wins (prd-bus-routing §5.4); earlier calls are dropped with W203.
    //     master(c) was recorded as mixer(0, c), so master and an explicit
    //     mixer(0, …) collide naturally.
    std::map<int, MixerCall> winning_mixers;
    for (const auto& mc : mixer_calls_) {
        auto it = winning_mixers.find(mc.bus_index);
        if (it != winning_mixers.end()) {
            warn("W203",
                 "mixer(" + std::to_string(mc.bus_index) + ")/master "
                 "overridden — the closure at line " +
                 std::to_string(it->second.call_loc.line) +
                 " is dropped; the call at line " +
                 std::to_string(mc.call_loc.line) + " takes effect",
                 it->second.call_loc);
        }
        winning_mixers[mc.bus_index] = mc;
    }
    // W205: a non-zero-bus mixer whose bus has no out()/bus() writers
    // processes silence (the prologue still clears the buffer, so it is safe).
    for (const auto& [idx, mc] : winning_mixers) {
        if (idx != 0 && writer_indices.find(idx) == writer_indices.end()) {
            warn("W205",
                 "mixer(" + std::to_string(idx) + ") targets a bus with no "
                 "writers — the closure processes silence", mc.call_loc);
        }
    }

    // 2. Allocate a stereo scratch pair per bus index (ascending order).
    std::map<int, std::uint16_t> bus_left;
    for (int idx : indices) {
        std::uint16_t l = buffers_.allocate();
        std::uint16_t r = buffers_.allocate();
        if (l == BufferAllocator::BUFFER_UNUSED ||
            r == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted (bus routing)", {});
            return;
        }
        if (r != l + 1) {
            error("E166",
                  "Internal error: bus buffer allocation not adjacent", {});
            return;
        }
        bus_left[idx] = l;
    }

    // 3. Rewrite bus placeholders to real left-buffer indices.
    auto fixup = [&](std::vector<cedar::Instruction>& code) {
        for (auto& inst : code) {
            if (inst.opcode == cedar::Opcode::OUTPUT &&
                is_placeholder(inst.out_buffer)) {
                inst.out_buffer = bus_left[inst.out_buffer - kPlaceLo];
            }
        }
    };
    fixup(instructions_);
    for (auto& desc : subprograms_) fixup(desc.body);

    const std::uint16_t bus0_l = bus_left[0];
    const std::uint16_t bus0_r = static_cast<std::uint16_t>(bus0_l + 1);

    // 4. Allocate epilogue constant buffers. The chain processes bus 0 in
    //    place, so only two PUSH_CONST scratch slots are needed: `c1` is
    //    reused (soft-clip threshold, then the clamp lower bound) since the
    //    threshold is dead by the time the clamp runs. allocate() is
    //    monotonic — if the last one succeeds all did.
    const std::uint16_t c1 = buffers_.allocate();
    const std::uint16_t c2 = buffers_.allocate();
    if (c2 == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted (bus epilogue)", {});
        return;
    }

    auto emit_push_const = [&](std::uint16_t dst, float value) {
        cedar::Instruction push{};
        push.opcode = cedar::Opcode::PUSH_CONST;
        push.out_buffer = dst;
        push.inputs[0] = 0xFFFF;
        push.inputs[1] = 0xFFFF;
        push.inputs[2] = 0xFFFF;
        push.inputs[3] = 0xFFFF;
        push.inputs[4] = 0xFFFF;
        encode_const_value(push, value);
        emit(push);
    };

    // 5. Emit the epilogue into the main stream.
    // (a) For each non-zero bus: run its mixer closure (if any) on the bus
    //     signal in place, then sum the bus into bus 0.
    for (const auto& [idx, l] : bus_left) {
        if (idx == 0) continue;
        auto mit = winning_mixers.find(idx);
        if (mit != winning_mixers.end()) {
            inline_mixer_closure(mit->second, l,
                                 static_cast<std::uint16_t>(l + 1));
        }
        cedar::Instruction o{};
        o.opcode = cedar::Opcode::OUTPUT;
        o.out_buffer = bus0_l;
        o.inputs[0] = l;
        o.inputs[1] = static_cast<std::uint16_t>(l + 1);
        o.inputs[2] = 0xFFFF;
        o.inputs[3] = 0xFFFF;
        o.inputs[4] = 0xFFFF;
        o.flags = cedar::InstructionFlag::BUS_WRITE;
        emit(o);
    }

    // (b) Bus-0 tone chain: the master / mixer(0) closure if one was given,
    //     otherwise the default polynomial soft-clip @ 0.9. Either runs in
    //     place on bus 0; the forced safety stage below always follows.
    {
        auto mit = winning_mixers.find(0);
        if (mit != winning_mixers.end()) {
            inline_mixer_closure(mit->second, bus0_l, bus0_r);
        } else {
            emit_push_const(c1, 0.9f);         // c1 = soft-clip threshold
            cedar::Instruction soft{};
            soft.opcode = cedar::Opcode::DISTORT_SOFT;
            soft.out_buffer = bus0_l;          // in place: bus0_l, bus0_l+1
            soft.inputs[0] = bus0_l;           // STEREO_INPUT → reads bus0_l+1
            soft.inputs[1] = c1;
            soft.inputs[2] = 0xFFFF;           // dry  → default 0.0
            soft.inputs[3] = 0xFFFF;           // wet  → default 1.0
            soft.inputs[4] = 0xFFFF;
            soft.flags = static_cast<std::uint16_t>(
                cedar::InstructionFlag::STEREO_INPUT |
                cedar::InstructionFlag::STEREO_OUTPUT);
            soft.state_id = 0;
            emit(soft);
        }
    }

    // (c) Forced safety: hard rail at ±1.0 — "do not damage speakers".
    //     std::clamp() passes NaN through unchanged; the device-store
    //     OUTPUT below sanitizes any remaining NaN/Inf to 0. Together they
    //     guarantee the device never sees |sample| > 1.0 or a non-finite
    //     value, regardless of what the bus-0 chain produced.
    emit_push_const(c1, -1.0f);            // c1 reused: clamp lower bound
    emit_push_const(c2, 1.0f);             // c2: clamp upper bound
    emit(cedar::Instruction::make_ternary(cedar::Opcode::CLAMP, bus0_l,
                                          bus0_l, c1, c2));
    emit(cedar::Instruction::make_ternary(cedar::Opcode::CLAMP, bus0_r,
                                          bus0_r, c1, c2));

    // (d) Device store: a plain OUTPUT (no BUS_WRITE flag) accumulates into
    //     the device sinks and sanitizes NaN/Inf → 0 on the way.
    {
        cedar::Instruction o{};
        o.opcode = cedar::Opcode::OUTPUT;
        o.out_buffer = 0xFFFF;
        o.inputs[0] = bus0_l;
        o.inputs[1] = bus0_r;
        o.inputs[2] = 0xFFFF;
        o.inputs[3] = 0xFFFF;
        o.inputs[4] = 0xFFFF;
        emit(o);
    }

    // 6. Prologue: clear every bus accumulator to silence before any writer.
    //    Prepended to the main stream — safe because no opcode encodes an
    //    absolute instruction address (control flow uses relative offsets
    //    and the post-finalization subprogram table).
    std::vector<cedar::Instruction> prologue;
    for (const auto& [idx, l] : bus_left) {
        (void)idx;
        prologue.push_back(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, l, cedar::BUFFER_ZERO));
        prologue.push_back(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, static_cast<std::uint16_t>(l + 1),
            cedar::BUFFER_ZERO));
    }
    instructions_.insert(instructions_.begin(),
                         prologue.begin(), prologue.end());
    source_locations_.insert(source_locations_.begin(),
                             prologue.size(), SourceLocation{});

    // Prepending shifts every main-stream instruction index. Patch the
    // absolute-index metadata recorded during visit(). (Control-flow
    // offsets are relative and subprogram offsets are resolved after this,
    // so only scalar_sample_mappings_ needs fixing.)
    const auto shift = static_cast<std::uint32_t>(prologue.size());
    for (auto& m : scalar_sample_mappings_) {
        m.instruction_index += shift;
    }
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
            var_name = ctx_->interner->view(n.as_identifier());
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

bool CodeGenerator::reorder_spread_named_args(const BuiltinInfo& builtin,
                                              const std::string& func_name,
                                              std::vector<ExpandedArg>& args,
                                              SourceLocation call_loc) {
    if (args.empty()) return true;

    struct ArgInfo {
        std::size_t arg_idx;              // index into args
        std::optional<std::string> name;  // none → positional
        int target_pos;                   // slot index, -1 = dropped/unknown
    };
    std::vector<ArgInfo> infos;
    infos.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        infos.push_back({i, args[i].name, -1});
    }

    // Resolve each argument to a target slot. Positional args fill 0,1,2…;
    // named args map by find_param() into the unified regular+extended index
    // space. Unknown field names are warned (W160) and dropped.
    bool has_named = false;
    bool seen_named = false;
    std::set<std::string> used_params;
    for (std::size_t i = 0; i < infos.size(); ++i) {
        if (infos[i].name.has_value()) {
            has_named = true;
            seen_named = true;
            const std::string& name = *infos[i].name;

            if (!used_params.insert(name).second) {
                error("E010", "Duplicate named argument '" + name +
                      "' in call to '" + func_name + "'",
                      args[infos[i].arg_idx].loc);
                return false;
            }

            int param_idx = builtin.find_param(name);
            if (param_idx < 0) {
                // PRD decision: an unknown spread field is non-fatal — warn
                // and drop it, matching the user-function spread path.
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
                warn("W160", "Spread field '" + name +
                     "' has no matching parameter in " + func_name +
                     "(" + sig + ")", args[infos[i].arg_idx].loc);
                continue;  // target_pos stays -1 → dropped
            }
            infos[i].target_pos = param_idx;
        } else {
            if (seen_named) {
                error("E009", "Positional argument cannot follow named argument "
                      "in call to '" + func_name + "'",
                      args[infos[i].arg_idx].loc);
                return false;
            }
            infos[i].target_pos = static_cast<int>(i);
        }
    }

    if (!has_named) return true;  // already positional — nothing to reorder

    // A named arg must not collide with a positional one filling the same slot.
    for (std::size_t i = 0; i < infos.size(); ++i) {
        if (infos[i].name.has_value()) continue;
        for (std::size_t j = 0; j < infos.size(); ++j) {
            if (infos[j].name.has_value() && infos[j].target_pos >= 0 &&
                infos[j].target_pos == static_cast<int>(i)) {
                error("E012", "Parameter '" + *infos[j].name + "' at position " +
                      std::to_string(i) + " conflicts with positional argument "
                      "in call to '" + func_name + "'",
                      args[infos[i].arg_idx].loc);
                return false;
            }
        }
    }

    // Build the canonical-order slot vector.
    int max_pos = -1;
    for (const auto& info : infos) {
        if (info.target_pos > max_pos) max_pos = info.target_pos;
    }
    if (max_pos < 0) {
        // Every argument was a dropped unknown field — emit an empty call.
        args.clear();
        return true;
    }
    // -1 = empty (needs underscore gap-fill); ≥0 = source index into `args`.
    std::vector<int> slot_source(static_cast<std::size_t>(max_pos) + 1, -1);
    for (const auto& info : infos) {
        if (info.target_pos >= 0) {
            slot_source[static_cast<std::size_t>(info.target_pos)] =
                static_cast<int>(info.arg_idx);
        }
    }

    // Materialise the slot-ordered ExpandedArg vector. Names are positional
    // after reorder — clear them so downstream logic ignores them. Underscore
    // gap-fills carry call_loc and is_underscore=true; the per-arg loop turns
    // each into the parameter's declared default (or E106).
    std::vector<ExpandedArg> reordered;
    reordered.reserve(slot_source.size());
    for (std::size_t i = 0; i < slot_source.size(); ++i) {
        if (slot_source[i] < 0) {
            ExpandedArg gap;
            gap.name = std::nullopt;
            gap.source_node = NULL_NODE;
            gap.resolved = std::nullopt;
            gap.loc = call_loc;
            gap.is_underscore = true;
            reordered.push_back(std::move(gap));
        } else {
            ExpandedArg moved = std::move(args[static_cast<std::size_t>(slot_source[i])]);
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
