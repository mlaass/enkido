// Pattern and chord codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/codegen/state_init_builder.hpp"
#include "akkado/codegen/options.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/pattern_debug.hpp"
#include "akkado/tuning.hpp"
#include "akkado/voicing.hpp"
#include "pattern_compiler.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/event_transform_encoding.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::SamplePatternEmitCtx;
using codegen::emit_sample_chain;
using codegen::sample_refs_from_mappings;
using codegen::mini_content_location;
using codegen::get_pattern_arg;
using codegen::get_number_arg;
using codegen::get_string_arg;
using codegen::is_pattern_node;

// All SAMPLE_PLAY emission goes through emit_sample_chain, defined in
// akkado/codegen/helpers.hpp. See docs/prd-sample-emission-unification.md.

// Handle MiniLiteral (pattern) nodes
TypedValue CodeGenerator::handle_mini_literal(NodeIndex node, const Node& n) {
    // PRD Phase 1b: MiniLiteralData replaces the legacy StringData(mode) +
    // first-child layout. The parsed mini-AST lives in a per-literal
    // sub-arena referenced via `mini_arena`.
    const auto& lit_data = n.as_mini_literal();
    if (lit_data.mode_marker == "timeline") {
        return handle_timeline_literal(node, n);
    }

    if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
        error("E114", "Pattern has no parsed content", n.location);
        return TypedValue::void_val();
    }
    const AstArena& mini_arena = *lit_data.mini_arena;
    NodeIndex pattern_node = lit_data.mini_root;

    // PRD prd-remove-pat-builtin §6.3: keep "pat" path segments unchanged
    // to preserve hot-swap semantic-ID hashes across the migration.
    std::uint32_t pat_count = call_counters_["pat"]++;
    push_path("pat#" + std::to_string(pat_count));
    std::uint32_t state_id = compute_state_id();

    // Use the SequenceCompiler for lazy queryable patterns
    SequenceCompiler compiler(mini_arena, sample_registry_);
    // Set base offset so event source_offset values are pattern-relative
    const Node& pattern = mini_arena[pattern_node];
    compiler.set_pattern_base_offset(pattern.location.offset);
    if (!compiler.compile(pattern_node)) {
        // Empty pattern - emit zero
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        pop_path();
        return cache_and_return(node, TypedValue::signal(out));
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // cycle_length is per-sequence in beats. The compiler computes it from
    // the sum of top-level element weights (see compile_top_level_pattern).
    float cycle_length = compiler.cycle_length();

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = alloc_buffer(n.location);
    std::uint16_t velocity_buf = alloc_buffer(n.location);
    std::uint16_t trigger_buf = alloc_buffer(n.location);

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction (queries pattern at block boundaries)
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_QUERY)
        .output(0xFFFF)  // No direct output
        .state_id(state_id)
        .emit(*this);

    // Check for polyphonic patterns (chords with multiple values per event)
    std::uint8_t max_voices = compiler.max_voices();

    // Track polyphonic patterns for error reporting (must be consumed by poly())
    if (max_voices > 1 && !is_sample_pattern) {
        polyphonic_pattern_nodes_[node] = {n.location, max_voices, state_id};
    }

    // Emit single-voice SEQPAT_STEP/GATE/TYPE (voice 0 only)
    auto pattern_payload = emit_per_voice_seqpat(node, state_id, max_voices, value_buf, velocity_buf,
                                                  trigger_buf, is_sample_pattern, n.location);
    if (!pattern_payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties
    // collected by the SequenceCompiler from `c4{cutoff:0.3}`-style suffixes.
    if (!emit_custom_property_buffers(compiler, *pattern_payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Store sequence program initialization data
    auto seq_init = codegen::StateInitBuilder::sequence_program(state_id)
                        .cycle_length(cycle_length)
                        .sequences(compiler.sequences())
                        .sequence_events(compiler.sequence_events())
                        .total_events(compiler.total_events())
                        .is_sample_pattern(is_sample_pattern)
                        .pattern_location(pattern.location)
                        .sequence_sample_mappings(compiler.sample_mappings());
    if (emit_debug_json_) {
        seq_init.ast_json(serialize_mini_ast_json(pattern_node, mini_arena));
    }
    seq_init.publish(*this);

    pattern_payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(pattern_payload->sample_refs);

    std::uint16_t result_buf = value_buf;

    // Handle sample patterns - need to wire to SAMPLE_PLAY
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = n.location;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::signal(value_buf);  // Return value buffer as fallback
        }
        result_buf = output_buf;
    }

    pop_path();

    pattern_payload->state_id = state_id;
    pattern_payload->cycle_length = cycle_length;
    return cache_and_return(node, TypedValue::make_pattern(pattern_payload, result_buf));
}


// Emit single-voice SEQPAT_STEP for voice 0 (plus extra SEQPAT_STEPs for
// chord voices), then delegate the extended-field allocation/emission to
// emit_extended_field_buffers() so every pattern producer shares the same
// records-and-field-access PRD §3.1–§3.3 wiring.
std::shared_ptr<PatternPayload> CodeGenerator::emit_per_voice_seqpat(NodeIndex node, std::uint32_t state_id,
                                           std::uint8_t max_voices,
                                           std::uint16_t value_buf, std::uint16_t velocity_buf,
                                           std::uint16_t trigger_buf,
                                           bool is_sample_pattern, SourceLocation loc,
                                           std::uint16_t clock_override) {
    (void)node;
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_STEP)
        .inputs({velocity_buf, trigger_buf, /*voice*/ 0, clock_override})
        .output(value_buf)
        .state_id(state_id)
        .emit(*this);

    // Per-voice freq buffers for chord polyphony. voice_freqs[0] = value_buf
    // (voice 0 already emitted above). Allocate and emit one extra SEQPAT_STEP
    // per chord voice so consumers like soundfont can read every voice's freq.
    // Velocity/trigger stay on voice 0 (inputs[0..1] unused here).
    std::vector<std::uint16_t> voice_freqs;
    if (max_voices > 1 && !is_sample_pattern) {
        voice_freqs.reserve(max_voices);
        voice_freqs.push_back(value_buf);
        for (std::uint8_t v = 1; v < max_voices; ++v) {
            const std::uint16_t v_buf =
                codegen::InstructionBuilder(cedar::Opcode::SEQPAT_STEP)
                    .input(2, v)  // voice index
                    .input(3, clock_override)
                    .state_id(state_id)
                    .emit(*this, loc);
            if (v_buf == BufferAllocator::BUFFER_UNUSED) {
                return nullptr;
            }
            voice_freqs.push_back(v_buf);
        }
    }

    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL]  = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    if (!emit_extended_field_buffers(*payload, state_id, loc, clock_override)) {
        error("E101", "Buffer pool exhausted", loc);
        return nullptr;
    }
    payload->is_sample_pattern = is_sample_pattern;
    payload->max_voices = max_voices;
    payload->voice_freqs = std::move(voice_freqs);
    return payload;
}

// Phase 2.1 PRD §11: emit one SEQPAT_PROP per registered custom-property slot,
// allocate a buffer per slot, and populate payload->custom_fields. The slot
// indices come from SequenceCompiler::custom_property_slots(), which is
// populated by record-suffix keys in MiniAtomData.properties (and by
// standalone bend()/aftertouch() transforms in compile_pattern_for_transform).
bool CodeGenerator::emit_custom_property_buffers(
    const SequenceCompiler& compiler,
    PatternPayload& payload,
    std::uint32_t state_id,
    std::uint16_t clock_override) {
    for (const auto& [key, slot] : compiler.custom_property_slots()) {
        std::uint16_t buf = buffers_.allocate();
        if (buf == BufferAllocator::BUFFER_UNUSED) return false;
        codegen::InstructionBuilder(cedar::Opcode::SEQPAT_PROP)
            .inputs({/*voice*/ 0, clock_override})
            .output(buf)
            .rate(slot)
            .state_id(state_id)
            .emit(*this);
        payload.custom_fields[key] = buf;
        // Phase 3: also record the runtime prop slot so handle_poly_call can
        // plumb this custom field into the per-voice field bank.
        payload.custom_field_slots[key] = slot;
    }
    return true;
}

// Allocate the 8 extended pattern-field buffers and emit SEQPAT_GATE/TYPE/
// FIELD/PHASE for voice 0. SEQPAT_FIELD selectors must match op_seqpat_field
// in cedar/include/cedar/opcodes/sequencing.hpp (0=dur, 1=chance, 2=time,
// 3=note, 4=sample_id). Records-and-field-access PRD §3.1–§3.3.
bool CodeGenerator::emit_extended_field_buffers(
    PatternPayload& payload,
    std::uint32_t state_id,
    SourceLocation loc,
    std::uint16_t clock_override) {
    (void)loc;  // reserved for future per-instruction location plumbing
    std::uint16_t gate_buf      = buffers_.allocate();
    std::uint16_t type_buf      = buffers_.allocate();
    std::uint16_t note_buf      = buffers_.allocate();
    std::uint16_t dur_buf       = buffers_.allocate();
    std::uint16_t chance_buf    = buffers_.allocate();
    std::uint16_t time_buf      = buffers_.allocate();
    std::uint16_t phase_buf     = buffers_.allocate();
    std::uint16_t sample_id_buf = buffers_.allocate();

    if (gate_buf == BufferAllocator::BUFFER_UNUSED ||
        type_buf == BufferAllocator::BUFFER_UNUSED ||
        note_buf == BufferAllocator::BUFFER_UNUSED ||
        dur_buf == BufferAllocator::BUFFER_UNUSED ||
        chance_buf == BufferAllocator::BUFFER_UNUSED ||
        time_buf == BufferAllocator::BUFFER_UNUSED ||
        phase_buf == BufferAllocator::BUFFER_UNUSED ||
        sample_id_buf == BufferAllocator::BUFFER_UNUSED) {
        return false;
    }

    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_GATE)
        .inputs({/*voice*/ 0, clock_override})
        .output(gate_buf)
        .state_id(state_id)
        .emit(*this);

    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_TYPE)
        .inputs({/*voice*/ 0, clock_override})
        .output(type_buf)
        .state_id(state_id)
        .emit(*this);

    auto emit_field = [&](std::uint16_t out_buf, std::uint8_t selector) {
        codegen::InstructionBuilder(cedar::Opcode::SEQPAT_FIELD)
            .inputs({/*voice*/ 0, clock_override})
            .output(out_buf)
            .rate(selector)
            .state_id(state_id)
            .emit(*this);
    };
    emit_field(dur_buf,       0);
    emit_field(chance_buf,    1);
    emit_field(time_buf,      2);
    emit_field(note_buf,      3);
    emit_field(sample_id_buf, 4);

    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_PHASE)
        .inputs({/*voice*/ 0, clock_override})
        .output(phase_buf)
        .state_id(state_id)
        .emit(*this);

    payload.fields[PatternPayload::GATE]      = gate_buf;
    payload.fields[PatternPayload::TYPE]      = type_buf;
    payload.fields[PatternPayload::NOTE]      = note_buf;
    payload.fields[PatternPayload::DUR]       = dur_buf;
    payload.fields[PatternPayload::CHANCE]    = chance_buf;
    payload.fields[PatternPayload::TIME]      = time_buf;
    payload.fields[PatternPayload::PHASE]     = phase_buf;
    payload.fields[PatternPayload::SAMPLE_ID] = sample_id_buf;
    return true;
}

// Handle pattern variable reference
TypedValue CodeGenerator::handle_pattern_reference(const std::string& name,
                                                    NodeIndex pattern_node,
                                                    SourceLocation loc) {
    if (pattern_node == NULL_NODE) {
        error("E123", "Pattern variable '" + name + "' has invalid pattern node", loc);
        return TypedValue::void_val();
    }

    const Node& pattern_n = ast_->arena[pattern_node];
    if (pattern_n.type != NodeType::MiniLiteral) {
        // Bindings whose RHS is a transform-on-pattern or a pattern-producer
        // call (e.g. `notes = n"…".slow(2)` or `notes = note("c d")`) arrive
        // here as Call nodes after the analyzer's method-call desugaring.
        // Delegate to the regular visit machinery — the transform / producer
        // handlers (handle_slow_call, handle_note_call, etc.) know how to
        // turn them into the correct sequenced TypedValue. Push the binding
        // name onto the path so internal state_ids stay tied to the name
        // for hot-swap state preservation.
        if (pattern_n.type == NodeType::Call ||
            pattern_n.type == NodeType::MethodCall) {
            push_path(name);
            TypedValue result = visit(pattern_node);
            pop_path();
            return result;
        }
        error("E124", "Pattern variable '" + name + "' does not refer to a pattern", loc);
        return TypedValue::void_val();
    }

    push_path(name);
    std::uint32_t state_id = compute_state_id();

    // PRD Phase 1b: the parsed mini-AST lives in MiniLiteralData's sub-arena.
    const auto& lit_data = pattern_n.as_mini_literal();
    if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
        error("E114", "Pattern has no parsed content", loc);
        pop_path();
        return TypedValue::void_val();
    }
    const AstArena& mini_arena = *lit_data.mini_arena;
    NodeIndex mini_pattern = lit_data.mini_root;

    // Use the SequenceCompiler
    SequenceCompiler compiler(mini_arena, sample_registry_);
    if (!compiler.compile(mini_pattern)) {
        // Empty pattern - emit zero
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
        }
        pop_path();
        return TypedValue::signal(out);
    }

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // cycle_length is per-sequence in beats. The compiler computes it from
    // the sum of top-level element weights (see compile_top_level_pattern).
    float cycle_length = compiler.cycle_length();

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers
    std::uint16_t value_buf = alloc_buffer(loc);
    std::uint16_t velocity_buf = alloc_buffer(loc);
    std::uint16_t trigger_buf = alloc_buffer(loc);

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_QUERY)
        .output(0xFFFF)
        .state_id(state_id)
        .emit(*this);

    // Emit single-voice SEQPAT_STEP/GATE/TYPE
    std::uint8_t max_voices = compiler.max_voices();

    // Track polyphonic patterns for error reporting (must be consumed by poly())
    if (max_voices > 1 && !is_sample_pattern) {
        polyphonic_pattern_nodes_[pattern_node] = {loc, max_voices, state_id};
    }

    auto pattern_payload = emit_per_voice_seqpat(pattern_node, state_id, max_voices, value_buf, velocity_buf,
                                                  trigger_buf, is_sample_pattern, loc);
    if (!pattern_payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties.
    if (!emit_custom_property_buffers(compiler, *pattern_payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", loc);
        return TypedValue::void_val();
    }

    // Store sequence program
    codegen::StateInitBuilder::sequence_program(state_id)
        .cycle_length(cycle_length)
        .sequences(compiler.sequences())
        .sequence_events(compiler.sequence_events())
        .total_events(compiler.total_events())
        .is_sample_pattern(is_sample_pattern)
        .sequence_sample_mappings(compiler.sample_mappings())
        .publish(*this);

    pattern_payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(pattern_payload->sample_refs);

    // Wire up SAMPLE_PLAY for sample patterns. Without this the returned
    // buffer would be raw sample-IDs (DC), not audio.
    std::uint16_t result_buf = value_buf;
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = loc;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            pop_path();
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    pop_path();
    pattern_payload->state_id = state_id;
    pattern_payload->cycle_length = cycle_length;
    return cache_and_return(pattern_node, TypedValue::make_pattern(pattern_payload, result_buf));
}

// Handle chord() calls - uses SEQPAT system via SequenceCompiler
TypedValue CodeGenerator::handle_chord_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E125", "chord() requires exactly 1 argument", n.location);
        return TypedValue::void_val();
    }

    const Node& arg_node = ast_->arena[arg];
    NodeIndex str_node = (arg_node.type == NodeType::Argument) ? arg_node.first_child : arg;

    if (str_node == NULL_NODE) {
        error("E125", "chord() requires a string argument", n.location);
        return TypedValue::void_val();
    }

    const Node& str_n = ast_->arena[str_node];
    if (str_n.type != NodeType::StringLit) {
        error("E126", "chord() argument must be a string literal (e.g., \"Am\", \"C7 F G\")",
              str_n.location);
        return TypedValue::void_val();
    }

    std::string chord_str = str_n.as_string();

    // PRD Phase 1b: parse into a codegen-owned scratch arena instead of
    // mutating ast_->arena via const_cast. The scratch arena lives until
    // CodeGenerator is destroyed, anchoring SequenceCompiler's traversal.
    auto scratch = std::make_shared<AstArena>();
    auto [pattern_root, diags] = parse_mini(chord_str, *scratch,
                                            mini_content_location(str_n.location),
                                            /*sample_only=*/false);
    codegen_mini_arenas_.push_back(scratch);

    // Report any parse errors
    for (const auto& diag : diags) {
        if (diag.severity == Severity::Error) {
            diagnostics_.push_back(diag);
        }
    }

    if (pattern_root == NULL_NODE) {
        error("E127", "Failed to parse chord pattern: \"" + chord_str + "\"", str_n.location);
        return TypedValue::void_val();
    }

    // Use SequenceCompiler to compile the chord pattern (same as pat())
    std::uint32_t chord_count = call_counters_["chord"]++;
    push_path("chord#" + std::to_string(chord_count));
    std::uint32_t state_id = compute_state_id();

    SequenceCompiler compiler(*scratch, sample_registry_);
    const Node& pattern = (*scratch)[pattern_root];
    compiler.set_pattern_base_offset(pattern.location.offset);

    if (!compiler.compile(pattern_root)) {
        error("E127", "Failed to compile chord pattern: \"" + chord_str + "\"", str_n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // cycle_length is per-sequence in beats. The compiler computes it from
    // the sum of top-level element weights (see compile_top_level_pattern).
    float cycle_length = compiler.cycle_length();

    // Allocate buffers for outputs
    std::uint16_t value_buf = alloc_buffer(n.location);
    std::uint16_t velocity_buf = alloc_buffer(n.location);
    std::uint16_t trigger_buf = alloc_buffer(n.location);

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_QUERY)
        .output(0xFFFF)
        .state_id(state_id)
        .emit(*this);

    // Check for polyphonic patterns (chords with multiple values per event)
    std::uint8_t max_voices = compiler.max_voices();

    // Track polyphonic patterns for error reporting. Consumers that handle
    // chord polyphony natively (e.g. soundfont) erase this entry; mono synth
    // chains still produce E410.
    if (max_voices > 1) {
        polyphonic_pattern_nodes_[node] = {n.location, max_voices, state_id};
    }

    // Emit per-voice SEQPAT_STEP plus the voice-0 GATE/TYPE/FIELD/PHASE block.
    // emit_per_voice_seqpat populates payload->voice_freqs when max_voices > 1
    // so consumers can iterate every chord voice.
    auto payload = emit_per_voice_seqpat(node, state_id, max_voices, value_buf,
                                          velocity_buf, trigger_buf,
                                          /*is_sample_pattern=*/false, n.location);
    if (!payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Store sequence program initialization data
    codegen::StateInitBuilder::sequence_program(state_id)
        .cycle_length(cycle_length)
        .sequences(compiler.sequences())
        .sequence_events(compiler.sequence_events())
        .total_events(compiler.total_events())
        .is_sample_pattern(false)
        .pattern_location(pattern.location)
        .sequence_sample_mappings(compiler.sample_mappings())
        .publish(*this);

    payload->state_id = state_id;
    payload->cycle_length = cycle_length;

    // Phase 2.1 PRD §11: chord patterns can carry record-suffix properties too
    // (e.g. `chord("Am{velmod:0.5}")`). Surface them via SEQPAT_PROP.
    if (!emit_custom_property_buffers(compiler, *payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(payload->sample_refs);

    pop_path();
    return cache_and_return(node, TypedValue::make_pattern(payload, value_buf));
}

// PRD prd-patterns-as-scalar-values §5.6: explicit Pattern→Signal cast.
// scalar(p) returns p.freq for monophonic non-sample patterns; idempotent
// on a Signal arg. Sample / polyphonic patterns error E161.
TypedValue CodeGenerator::handle_scalar_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E125", "scalar() takes exactly one argument", n.location);
        return TypedValue::error_val();
    }
    const Node& arg_n = ast_->arena[arg];
    NodeIndex inner = (arg_n.type == NodeType::Argument) ? arg_n.first_child : arg;
    if (inner == NULL_NODE) {
        error("E125", "scalar() takes exactly one argument", n.location);
        return TypedValue::error_val();
    }

    TypedValue tv = visit(inner);
    if (tv.error) return tv;

    // Idempotent on Signal/Number — return as-is so scalar(scalar(p)) is safe.
    if (tv.type == ValueType::Signal || tv.type == ValueType::Number) {
        return cache_and_return(node, tv);
    }

    if (tv.type != ValueType::Pattern || !tv.pattern) {
        error("E161", std::string("scalar() expects a Pattern or Signal, got ") +
                          value_type_name(tv.type),
              n.location);
        return TypedValue::error_val();
    }
    if (tv.pattern->is_sample_pattern) {
        error("E161",
              "scalar() cannot cast a sample pattern; pick a field explicitly (e.g. p.type)",
              n.location);
        return TypedValue::error_val();
    }
    if (tv.pattern->max_voices > 1) {
        error("E161",
              "scalar() cannot cast a polyphonic pattern; use poly() or pick a voice explicitly",
              n.location);
        return TypedValue::error_val();
    }
    std::uint16_t buf = tv.pattern->fields[PatternPayload::FREQ];
    if (buf == 0xFFFF) {
        error("E161", "scalar() pattern has no value buffer", n.location);
        return TypedValue::error_val();
    }
    return cache_and_return(node, TypedValue::signal(buf));
}

// ============================================================================
// Timeline curve literal codegen
// ============================================================================

TypedValue CodeGenerator::handle_timeline_literal(NodeIndex node, const Node& n) {
    // PRD Phase 1b: curve pattern lives in MiniLiteralData's sub-arena.
    const auto& lit_data = n.as_mini_literal();
    if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
        error("E114", "Timeline curve has no parsed content", n.location);
        return TypedValue::void_val();
    }
    const AstArena& mini_arena = *lit_data.mini_arena;
    NodeIndex pattern_node = lit_data.mini_root;

    // Evaluate the curve pattern to events
    PatternEvaluator evaluator(mini_arena);
    PatternEventStream stream = evaluator.evaluate(pattern_node, 0);

    // Convert events to breakpoints
    auto breakpoints = events_to_breakpoints(stream.events);
    if (breakpoints.empty()) {
        // Empty curve - emit zero
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        return cache_and_return(node, TypedValue::signal(out));
    }

    if (breakpoints.size() > cedar::TimelineState::MAX_BREAKPOINTS) {
        warn("W200", "Timeline curve exceeds 64 breakpoints, truncating", n.location);
        breakpoints.resize(cedar::TimelineState::MAX_BREAKPOINTS);
    }

    // Allocate state and output buffer, emit TIMELINE instruction
    std::uint32_t tl_count = call_counters_["timeline"]++;
    push_path("timeline#" + std::to_string(tl_count));
    std::uint32_t state_id = compute_state_id();
    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::TIMELINE)
            .state_id(state_id)
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    codegen::StateInitBuilder::timeline(state_id)
        .timeline_breakpoints(std::move(breakpoints))
        .timeline_loop(true, stream.cycle_span)  // cycle = beat
        .publish(*this);

    pop_path();
    return cache_and_return(node, TypedValue::signal(out_buf));
}

// ============================================================================
// Timeline function call form: timeline("__/''")
// ============================================================================

TypedValue CodeGenerator::handle_timeline_call(NodeIndex node, const Node& n) {
    // Extract the first string argument
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E114", "timeline() requires a string argument", n.location);
        return TypedValue::void_val();
    }

    const Node& arg_node = ast_->arena[arg];
    NodeIndex arg_value = arg;
    if (arg_node.type == NodeType::Argument) {
        arg_value = arg_node.first_child;
    }
    if (arg_value == NULL_NODE) {
        error("E114", "timeline() requires a string argument", n.location);
        return TypedValue::void_val();
    }

    const Node& value_node = ast_->arena[arg_value];
    if (value_node.type != NodeType::StringLit) {
        error("E114", "timeline() argument must be a string literal", n.location);
        return TypedValue::void_val();
    }

    std::string curve_str = value_node.as_string();

    // PRD Phase 1b: parse into a codegen scratch arena instead of mutating
    // ast_->arena via const_cast.
    AstArena& scratch = acquire_mini_scratch_arena();
    auto [pattern_root, diags] = parse_mini(curve_str, scratch,
        mini_content_location(value_node.location), false, true);

    for (const auto& d : diags) {
        if (d.severity == Severity::Error) {
            error("E114", d.message, n.location);
        } else {
            warn("W200", d.message, n.location);
        }
    }

    if (pattern_root == NULL_NODE) {
        error("E114", "timeline() failed to parse curve notation", n.location);
        return TypedValue::void_val();
    }

    // Evaluate the curve pattern to events
    PatternEvaluator evaluator(scratch);
    PatternEventStream stream = evaluator.evaluate(pattern_root, 0);

    // Convert events to breakpoints
    auto breakpoints = events_to_breakpoints(stream.events);
    if (breakpoints.empty()) {
        std::uint16_t out = emit_zero();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
        }
        return cache_and_return(node, TypedValue::signal(out));
    }

    if (breakpoints.size() > cedar::TimelineState::MAX_BREAKPOINTS) {
        warn("W200", "Timeline curve exceeds 64 breakpoints, truncating", n.location);
        breakpoints.resize(cedar::TimelineState::MAX_BREAKPOINTS);
    }

    // Allocate state and output buffer, emit TIMELINE instruction
    std::uint32_t tl_count = call_counters_["timeline"]++;
    push_path("timeline#" + std::to_string(tl_count));
    std::uint32_t state_id = compute_state_id();
    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::TIMELINE)
            .state_id(state_id)
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    codegen::StateInitBuilder::timeline(state_id)
        .timeline_breakpoints(std::move(breakpoints))
        .timeline_loop(true, stream.cycle_span)  // cycle = beat
        .publish(*this);

    pop_path();
    return cache_and_return(node, TypedValue::signal(out_buf));
}

TypedValue CodeGenerator::handle_rev_call(NodeIndex node, const Node& n) {
    // rev(pattern) — reverse event order. PRD Phase 4 runtime form: each
    // block, EVENT_REORDER(REV) reads upstream OutputEvents and re-times each
    // event with `new_time = cycle_length - t - dur` (clamp >= 0). Composes
    // with upstream EVENT_MAP (e.g. transpose); composition was broken under
    // the pre-Phase-4 compile-time path because compile_pattern_for_transform
    // didn't see runtime EVENT_MAP transforms.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "rev() requires a pattern as argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "rev() argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "rev",
                             cedar::EVENT_REORDER_REV, /*flags=*/0,
                             /*param0=*/0xFFFF, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

TypedValue CodeGenerator::handle_ply_call(NodeIndex node, const Node& n) {
    // ply(pattern, n) — PRD Phase 4 runtime form: EVENT_FANOUT(PLY) emits N
    // sub-events per upstream event, each with duration d/N. `n` is a
    // compile-time constant (it directly determines OutputEvents capacity).
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "ply() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131", "ply() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "ply() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint32_t n_int = static_cast<std::uint32_t>(*n_arg);
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_fanout_call(node, n, "ply",
                            cedar::EVENT_FANOUT_PLY, n_buf,
                            /*cycle_length_factor=*/1.0f,
                            /*capacity_factor=*/n_int);
}

TypedValue CodeGenerator::handle_linger_call(NodeIndex node, const Node& n) {
    // linger(pattern, frac) — PRD Phase 4 runtime form: EVENT_FANOUT(LINGER)
    // drops events with t >= frac and rescales survivors to [0, 1). The
    // downstream cycle_length is upstream * frac, so the truncated segment
    // loops 1/frac times per upstream cycle (preserves legacy compile-time
    // semantics). frac is signal-rate ok; clamped to (0, 1] at runtime.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "linger() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "linger() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    auto frac_const = get_number_arg(*ast_, n, 1);
    if (frac_const.has_value() && *frac_const <= 0.0f) {
        error("E131", "linger() requires a positive frac (signal or constant)",
              n.location);
        return TypedValue::void_val();
    }

    std::uint16_t frac_buf = resolve_scalar_or_signal_arg(
        n, 1, "E131",
        "linger() requires a number or signal as second argument (frac)");
    if (frac_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();

    // For constants, fold the cycle_length factor at compile time so the
    // PatternPayload reports the correct cycle. For signals, leave the
    // compile-time factor at 1.0 (runtime mutates the SequenceState's
    // cycle_length each block per the LINGER body). This matches the
    // Phase 3 fast/slow precedent.
    const float clf = frac_const.has_value()
                          ? std::min(1.0f, static_cast<float>(*frac_const))
                          : 1.0f;
    return emit_fanout_call(node, n, "linger",
                            cedar::EVENT_FANOUT_LINGER, frac_buf,
                            clf,
                            /*capacity_factor=*/1u);
}

TypedValue CodeGenerator::handle_add_voicings_call(NodeIndex node, const Node& n) {
    (void)node;
    auto name_str = get_string_arg(*ast_, n, 0);
    if (!name_str.has_value()) {
        error("E131", "addVoicings() requires a name string as first argument", n.location);
        return TypedValue::void_val();
    }
    // Second argument should be a record literal {quality: [intervals], ...}.
    NodeIndex second = NULL_NODE;
    NodeIndex arg = n.first_child;
    int idx = 0;
    while (arg != NULL_NODE) {
        const Node& a = ast_->arena[arg];
        NodeIndex actual = arg;
        if (a.type == NodeType::Argument && a.first_child != NULL_NODE) actual = a.first_child;
        if (idx == 1) { second = actual; break; }
        ++idx;
        arg = a.next_sibling;
    }
    if (second == NULL_NODE) {
        error("E131", "addVoicings() requires a record literal {quality: [intervals], ...} as second argument", n.location);
        return TypedValue::void_val();
    }

    voicing::VoicingDict dict;
    dict.builtin_kind = -1;  // user dict — quality-table only
    const Node& rec = ast_->arena[second];
    if (rec.type != NodeType::RecordLit) {
        error("E131", "addVoicings() second argument must be a record literal", n.location);
        return TypedValue::void_val();
    }
    // Record fields are NodeType::Argument with RecordFieldData attached.
    NodeIndex field = rec.first_child;
    while (field != NULL_NODE) {
        const Node& f = ast_->arena[field];
        if (f.type == NodeType::Argument &&
            std::holds_alternative<Node::RecordFieldData>(f.data)) {
            const auto& fd = std::get<Node::RecordFieldData>(f.data);
            const std::string& key = fd.name;
            NodeIndex val_node = f.first_child;
            if (val_node != NULL_NODE) {
                const Node& vn = ast_->arena[val_node];
                if (vn.type == NodeType::ArrayLit) {
                    std::vector<int> intervals;
                    NodeIndex ele = vn.first_child;
                    while (ele != NULL_NODE) {
                        const Node& en = ast_->arena[ele];
                        if (std::holds_alternative<Node::NumberData>(en.data)) {
                            intervals.push_back(static_cast<int>(en.as_number()));
                        }
                        ele = en.next_sibling;
                    }
                    if (!key.empty()) dict.qualities[key] = std::move(intervals);
                }
            }
        }
        field = f.next_sibling;
    }

    ctx_->voicing_registry->define(*name_str, std::move(dict));
    return TypedValue::void_val();
}

} // namespace akkado
