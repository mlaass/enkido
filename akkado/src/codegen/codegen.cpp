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

#ifdef CEDAR_HOST_EXTENSIONS
// The `arg_index`-th argument of a Call node, if it is a string literal.
// (codegen_patterns.cpp has a pattern-node twin; this one walks a plain Call.)
static std::optional<std::string> get_call_string_arg(const Ast& ast, const Node& n,
                                                      std::size_t arg_index) {
    NodeIndex arg = n.first_child;
    for (std::size_t i = 0; arg != NULL_NODE; ++i, arg = ast.arena[arg].next_sibling) {
        if (i != arg_index) continue;
        const Node& unwrapped = ast.arena[unwrap_argument(ast.arena, arg)];
        if (unwrapped.type != NodeType::StringLit) return std::nullopt;
        return unwrapped.as_string();
    }
    return std::nullopt;
}
#endif

CodeGenerator::CodeGenerator(CompileContext& ctx, bool emit_debug_json)
    : ctx_(&ctx), emit_debug_json_(emit_debug_json) {}

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
    // ExtendedParams lives in a sibling StatePool slot keyed off the
    // opcode's state_id XOR'd with EXT_PARAMS_STATE_XOR. This keeps the
    // opcode's primary DSP state (e.g. ChorusState) and its ExtendedParams
    // in distinct slots — get_or_create<DSPState> would otherwise overwrite
    // the ExtendedParams.
    auto ext = codegen::StateInitBuilder::extended_params(
                   cedar::ext_params_state_id(state_id))
                   .ext_count(info.extended_param_count);

    for (std::uint8_t i = 0; i < info.extended_param_count && i < MAX_EXTENDED_PARAMS; ++i) {
        const std::size_t arg_idx = base + i;
        if (arg_idx < arg_buffers.size() &&
            arg_buffers[arg_idx] != BufferAllocator::BUFFER_UNUSED) {
            // Caller supplied an argument — read from its buffer at runtime.
            ext.ext_slot(i, 0.0f, arg_buffers[arg_idx]);
        } else if (!std::isnan(info.extended_defaults[i])) {
            // Use the declared default as a constant slot.
            ext.ext_slot(i, info.extended_defaults[i], 0xFFFFu);
        } else {
            // Required ext param missing — emit zero constant. The analyzer
            // is responsible for surfacing the missing-arg error upstream.
            ext.ext_slot(i, 0.0f, 0xFFFFu);
        }
    }

    ext.publish(*this);
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
#ifdef CEDAR_HOST_EXTENSIONS
    required_host_nodes_.clear();
#endif
    bus_buffers_.clear();
    bus_labels_.clear();
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
#ifdef CEDAR_HOST_EXTENSIONS
    result.required_host_nodes = std::move(required_host_nodes_);
#endif
    result.param_decls = std::move(param_decls_);
    result.viz_decls = std::move(viz_decls_);
    result.builtin_var_overrides = std::move(builtin_var_overrides_);
    result.required_wavetables = std::move(required_wavetables_);
    result.required_uris = std::move(required_uris_);
    result.required_buffers = buffers_.peak_count();
    result.bus_buffers = std::move(bus_buffers_);
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

std::uint16_t CodeGenerator::alloc_buffer(SourceLocation loc) {
    const std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", loc);
    }
    return out;
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

#ifdef CEDAR_HOST_EXTENSIONS
bool CodeGenerator::record_host_node_manifest(const cedar::Instruction& inst,
                                              const BuiltinInfo& builtin,
                                              const std::string& func_name,
                                              const Node& n,
                                              std::uint32_t seq_state_id) {
    const int slot = host_node_string_slot(func_name);
    if (slot < 0) return true;  // a plain host function, not a node

    auto name_arg = get_call_string_arg(*ast_, n, static_cast<std::size_t>(slot));
    if (!name_arg.has_value()) {
        error("E262", func_name + "() argument '" +
              std::string(builtin.param_names[static_cast<std::size_t>(slot)]) +
              "' must be a string literal", n.location);
        return false;
    }

    RequiredHostNode rec{inst.state_id, builtin.inst_rate, *name_arg};
    rec.seq_state_id = seq_state_id;

    // Open kwargs: the analyzer's reorder kept their names in the AST for
    // exactly this read (slots past the declared params). Child-list position
    // == input slot after reordering.
    if (host_node_open_kwargs(func_name)) {
        const std::size_t declared = static_cast<std::size_t>(builtin.input_count) +
                                     static_cast<std::size_t>(builtin.optional_count);
        std::size_t idx = 0;
        for (NodeIndex it_arg = n.first_child; it_arg != NULL_NODE;
             it_arg = ast_->arena[it_arg].next_sibling, ++idx) {
            const Node& arg_node = ast_->arena[it_arg];
            if (idx < declared || arg_node.type != NodeType::Argument) continue;
            auto kw = arg_node.as_arg_name();
            if (kw.has_value()) {
                rec.kwargs.push_back(HostNodeKwarg{static_cast<std::uint8_t>(idx), *kw});
            }
        }
    }

    required_host_nodes_.push_back(std::move(rec));
    return true;
}
#endif

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

} // namespace akkado
