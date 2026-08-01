// Visualization exposure codegen implementations
// Handles pianoroll(), oscilloscope(), waveform(), spectrum() for UI auto-generation
// These functions pass signal through while creating visualization metadata

#include "akkado/codegen.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/string_interner.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/options.hpp"
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/builtins.hpp"
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <algorithm>

namespace akkado {

using codegen::unwrap_argument;

// Helper: Extract optional string name argument
static std::string extract_name_arg(const AstArena& arena, NodeIndex arg_node,
                                     const std::string& default_name) {
    if (arg_node == NULL_NODE) {
        return default_name;
    }

    NodeIndex value_node = unwrap_argument(arena, arg_node);
    const Node& n = arena[value_node];

    if (n.type == NodeType::StringLit) {
        return std::string(n.as_string());
    }

    return default_name;
}

// Helper: Get next sibling argument
static NodeIndex next_arg(const AstArena& arena, NodeIndex arg_node) {
    if (arg_node == NULL_NODE) return NULL_NODE;
    return arena[arg_node].next_sibling;
}

// Helper: Extract a viz builtin's options record into a typed payload using
// the schema declared on the BuiltinInfo. Falls back to a permissive empty
// schema if the builtin has none — preserves the pre-Phase-5 behaviour for
// callers without a registered schema (none today, but defensive).
static codegen::OptionsPayload extract_viz_options(const AstArena& arena,
                                                    NodeIndex arg_node,
                                                    std::string_view builtin_name) {
    const OptionSchema* schema = nullptr;
    if (const BuiltinInfo* info = lookup_builtin(builtin_name)) {
        schema = info->find_option_schema(/* param_index = */ 2);
    }
    static const OptionSchema empty_schema{};
    return codegen::extract_options(arena, arg_node,
                                    schema ? *schema : empty_schema);
}

// Helper: quantize an FFT bin count to log2(N), with N constrained to the
// FFT_PROBE opcode's accepted set (256/512/1024/2048). Default = 1024.
static std::uint8_t fft_log2_from_payload(const codegen::OptionsPayload& payload,
                                           std::uint8_t default_log2 = 10) {
    auto fft = payload.get_number("fft");
    if (!fft) return default_log2;
    int v = static_cast<int>(*fft);
    if (v == 256)  return 8;
    if (v == 512)  return 9;
    if (v == 1024) return 10;
    if (v == 2048) return 11;
    return default_log2;
}

// ============================================================================
// pianoroll(signal, name?, options?) - Piano roll pattern visualization
// ============================================================================

TypedValue CodeGenerator::handle_pianoroll_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E170", "pianoroll() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer - pass through
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Piano Roll");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    auto options = extract_viz_options(ast_->arena, options_arg, "pianoroll");

    // 5. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::PianoRoll;
    decl.state_id = 0;  // Not used for piano roll
    decl.options_json = options.to_json();
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;

    // 6. Try to find corresponding pattern state_init for linking
    // The signal may come from a pattern - search state_inits for matching source location
    // This enables the piano roll to access pattern events without duplication
    decl.pattern_state_init_index = -1;
    for (std::size_t i = 0; i < state_inits_.size(); ++i) {
        const auto& init = state_inits_[i];
        if (init.type == StateInitData::Type::SequenceProgram) {
            // Check if pattern location is within or near this call
            // For now, use most recent pattern if signal comes from a pattern node
            decl.pattern_state_init_index = static_cast<std::int32_t>(i);
            // Store the pattern's state_id for direct lookup on the JS side
            decl.state_id = init.state_id;
        }
    }

    // 7. Add to declarations (no deduplication - multiple piano rolls allowed)
    viz_decls_.push_back(std::move(decl));

    // 8. Signal passes through unchanged
    return cache_and_return(node, TypedValue::signal(signal_buf));
}

// ============================================================================
// oscilloscope(signal, name?, options?) - Time-domain oscilloscope visualization
// ============================================================================

TypedValue CodeGenerator::handle_oscilloscope_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E171", "oscilloscope() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Oscilloscope");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    auto options = extract_viz_options(ast_->arena, options_arg, "oscilloscope");

    // 5. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("oscilloscope");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Oscilloscope;
    decl.state_id = state_id;
    decl.options_json = options.to_json();
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit PROBE opcode to capture signal data
    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::PROBE)
            .input(0, signal_buf)
            .state_id(state_id)
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// waveform(signal, name?, options?) - Time-domain waveform visualization (longer window)
// ============================================================================

TypedValue CodeGenerator::handle_waveform_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E172", "waveform() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Waveform");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    auto options = extract_viz_options(ast_->arena, options_arg, "waveform");

    // 5. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("waveform");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Waveform;
    decl.state_id = state_id;
    decl.options_json = options.to_json();
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit PROBE opcode to capture signal data
    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::PROBE)
            .input(0, signal_buf)
            .state_id(state_id)
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// spectrum(signal, name?, options?) - Frequency-domain FFT visualization
// ============================================================================

TypedValue CodeGenerator::handle_spectrum_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E173", "spectrum() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Spectrum");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    auto options = extract_viz_options(ast_->arena, options_arg, "spectrum");

    // 5. Generate state_id for probe buffer
    // Include source offset for uniqueness when multiple viz with same name exist
    push_path("spectrum");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Spectrum;
    decl.state_id = state_id;
    decl.options_json = options.to_json();
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit FFT_PROBE opcode (migrated from PROBE for WASM FFT)
    std::uint8_t fft_log2 = fft_log2_from_payload(options);

    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::FFT_PROBE)
            .rate(fft_log2)
            .input(0, signal_buf)
            .state_id(state_id)
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// waterfall(signal, name?, options?) - Spectral waterfall visualization
// ============================================================================

TypedValue CodeGenerator::handle_waterfall_call(NodeIndex node, const Node& n) {
    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error("E174", "waterfall() requires a signal argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex signal_node = unwrap_argument(ast_->arena, signal_arg);

    // 2. Visit signal to get its buffer
    std::uint16_t signal_buf = visit(signal_node).buffer;
    if (signal_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 3. Extract optional name argument
    NodeIndex name_arg = next_arg(ast_->arena, signal_arg);
    std::string name = extract_name_arg(ast_->arena, name_arg, "Waterfall");

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    auto options = extract_viz_options(ast_->arena, options_arg, "waterfall");

    // 5. Generate state_id for FFT probe
    push_path("waterfall");
    push_path(name);
    push_path(std::to_string(n.location.offset));
    std::uint32_t state_id = compute_state_id();
    pop_path();
    pop_path();
    pop_path();

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = VisualizationType::Waterfall;
    decl.state_id = state_id;
    decl.options_json = options.to_json();
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    viz_decls_.push_back(std::move(decl));

    // 7. Emit FFT_PROBE opcode
    std::uint8_t fft_log2 = fft_log2_from_payload(options);

    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::FFT_PROBE)
            .rate(fft_log2)
            .input(0, signal_buf)
            .state_id(state_id)
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

}  // namespace akkado
