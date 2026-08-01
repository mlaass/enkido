// Visualization exposure codegen — pianoroll(), oscilloscope(), waveform(),
// spectrum(), waterfall() for UI auto-generation. One data-driven emitter
// (PRD prd-codegen-sprawl-cleanup Phase 6): each builtin's BUILTIN_FUNCTIONS
// entry carries a VizMeta describing its widget type, default name,
// diagnostic code, and probe opcode; adding a visualizer is a table entry,
// not a new handler. Signal passes through while creating viz metadata.

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
// schema if the builtin has none — defensive; every registered viz has one.
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

// The canonical viz emit shape:
//   validate signal arg → visit → name → options → [state_id] →
//   push VisualizationDecl → [emit PROBE / FFT_PROBE]
// PianoRoll (probe == 0) emits no probe: it links to the pattern's own
// state_init instead and passes the signal buffer straight through.
TypedValue CodeGenerator::emit_visualization(NodeIndex node, const Node& n) {
    const std::string func_name{ctx_->interner->view(n.as_identifier())};
    const BuiltinInfo* info = lookup_builtin(func_name);
    if (info == nullptr || !info->viz_meta.has_value()) {
        error("E107", "Unknown visualization builtin: " + func_name, n.location);
        return TypedValue::error_val();
    }
    const VizMeta& meta = *info->viz_meta;

    // 1. Get signal argument (required)
    NodeIndex signal_arg = n.first_child;
    if (signal_arg == NULL_NODE) {
        error(meta.err_code,
              std::string(meta.name) + "() requires a signal argument",
              n.location);
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
    std::string name = extract_name_arg(ast_->arena, name_arg, meta.default_name);

    // 4. Extract optional options argument
    NodeIndex options_arg = next_arg(ast_->arena, name_arg);
    auto options = extract_viz_options(ast_->arena, options_arg, meta.name);

    // 5. Generate state_id for the probe buffer (probed widgets only).
    // Include source offset for uniqueness when multiple viz with same name
    // exist. PianoRoll links to the pattern's state_id instead (below).
    std::uint32_t state_id = 0;
    if (meta.probe != 0) {
        push_path(meta.name);
        push_path(name);
        push_path(std::to_string(n.location.offset));
        state_id = compute_state_id();
        pop_path();
        pop_path();
        pop_path();
    }

    // 6. Create visualization declaration
    VisualizationDecl decl;
    decl.name = name;
    decl.type = meta.type;
    decl.state_id = state_id;
    decl.options_json = options.to_json();
    decl.source_offset = n.location.offset;
    decl.source_length = n.location.length;
    decl.pattern_state_init_index = -1;

    if (meta.probe == 0) {
        // PianoRoll: link to a pattern state_init so the widget can read
        // pattern events without duplication. For now, use the most recent
        // SequenceProgram init.
        for (std::size_t i = 0; i < state_inits_.size(); ++i) {
            const auto& init = state_inits_[i];
            if (init.type == StateInitData::Type::SequenceProgram) {
                decl.pattern_state_init_index = static_cast<std::int32_t>(i);
                // Store the pattern's state_id for direct lookup on the JS side
                decl.state_id = init.state_id;
            }
        }
    }

    // 7. Add to declarations (no deduplication - multiple widgets allowed)
    viz_decls_.push_back(std::move(decl));

    // 8. PianoRoll: signal passes through unchanged, no probe emitted.
    if (meta.probe == 0) {
        return cache_and_return(node, TypedValue::signal(signal_buf));
    }

    // 9. Emit the probe opcode capturing signal data. FFT_PROBE carries
    // log2(fft bins) in the rate field.
    const cedar::Opcode probe_op =
        (meta.probe == 2) ? cedar::Opcode::FFT_PROBE : cedar::Opcode::PROBE;
    codegen::InstructionBuilder probe(probe_op);
    if (meta.probe == 2) {
        probe.rate(fft_log2_from_payload(options));
    }
    const std::uint16_t out_buf = probe.input(0, signal_buf)
                                      .state_id(state_id)
                                      .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

}  // namespace akkado
