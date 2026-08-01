#pragma once

// Helper functions for code generation
// These are commonly used patterns extracted for reuse and readability

#include "akkado/ast.hpp"
#include "akkado/codegen.hpp"
#include "akkado/string_interner.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <optional>
#include <set>
#include <vector>
#include <string>

namespace akkado {
namespace codegen {

// ============================================================================
// Instruction Encoding Helpers
// ============================================================================

/// Encode a float constant in a PUSH_CONST instruction.
/// The float is stored directly in state_id (32 bits).
[[gnu::always_inline]]
inline void encode_const_value(cedar::Instruction& inst, float value) {
    std::memcpy(&inst.state_id, &value, sizeof(float));
    inst.inputs[4] = 0xFFFF;  // BUFFER_UNUSED
}

// NOTE: The free `emit_push_const(buffers, instructions, value)` helper was
// removed in PRD prd-parser-codegen-correctness.md Phase 3 (F2). Use
// `CodeGenerator::emit_push_const(value)` instead — it routes through emit()
// so source_locations_ stays in sync.

// ============================================================================
// AST Navigation Helpers
// ============================================================================

/// Unwrap an Argument node to get the inner value node.
/// If the node is not an Argument, returns the node itself.
[[gnu::always_inline]]
inline NodeIndex unwrap_argument(const AstArena& arena, NodeIndex arg) {
    if (arg == NULL_NODE) return NULL_NODE;
    const Node& n = arena[arg];
    if (n.type == NodeType::Argument) {
        return n.first_child;
    }
    return arg;
}

/// Count the number of arguments in a Call node
[[gnu::always_inline]]
inline std::size_t count_call_args(const AstArena& arena, NodeIndex first_arg) {
    std::size_t count = 0;
    NodeIndex arg = first_arg;
    while (arg != NULL_NODE) {
        ++count;
        arg = arena[arg].next_sibling;
    }
    return count;
}

/// Information extracted from a Closure node
struct ClosureInfo {
    std::vector<std::string> params;
    NodeIndex body;
};

/// Return the body node of a Closure: structurally the LAST child.
/// The parser guarantees `Closure` children are laid out as
/// `[param₀, param₁, …, body]`, so the body is always the final child
/// regardless of its node type. (Earlier "skip Identifier children"
/// recovery silently consumed bare-identifier bodies like `(h) -> h`.)
[[gnu::always_inline]]
inline NodeIndex closure_body(const AstArena& arena, NodeIndex closure_node) {
    if (closure_node == NULL_NODE) return NULL_NODE;
    NodeIndex last = NULL_NODE;
    for (NodeIndex c = arena[closure_node].first_child; c != NULL_NODE;
         c = arena[c].next_sibling) {
        last = c;
    }
    return last;
}

/// Extract closure parameters and body from a Closure node. The body is the
/// last child (per `closure_body`); params are every preceding child that
/// is an `Identifier` (with either `ClosureParamData` or `IdentifierData`).
/// DestructureParam children are not returned here — callers that need them
/// walk children themselves with the same `child != body` bound.
[[gnu::always_inline]]
inline ClosureInfo extract_closure_info(const AstArena& arena, NodeIndex closure_node,
                                        const StringInterner& interner) {
    ClosureInfo info{};
    info.body = NULL_NODE;

    if (closure_node == NULL_NODE) return info;
    const Node& closure = arena[closure_node];
    if (closure.type != NodeType::Closure) return info;

    info.body = closure_body(arena, closure_node);
    if (info.body == NULL_NODE) return info;

    for (NodeIndex c = closure.first_child; c != info.body;
         c = arena[c].next_sibling) {
        const Node& cn = arena[c];
        if (cn.type != NodeType::Identifier) continue;
        if (std::holds_alternative<Node::ClosureParamData>(cn.data)) {
            info.params.push_back(cn.as_closure_param().name);
        } else if (std::holds_alternative<Node::IdentifierData>(cn.data)) {
            // PRD Phase 5 (F12): IdentifierData carries SymbolId; resolve
            // to std::string via the per-compile interner.
            info.params.push_back(std::string(interner.view(cn.as_identifier())));
        }
    }
    return info;
}

// ============================================================================
// Buffer Allocation Helpers
// ============================================================================

/// Allocate multiple buffers at once.
/// Returns true if all allocations succeeded, false otherwise.
/// On failure, the allocated buffers are still valid (no rollback).
template<typename... Args>
[[gnu::always_inline]]
inline bool allocate_buffers(BufferAllocator& alloc, std::uint16_t* first, Args*... rest) {
    *first = alloc.allocate();
    if (*first == BufferAllocator::BUFFER_UNUSED) {
        return false;
    }
    if constexpr (sizeof...(rest) > 0) {
        return allocate_buffers(alloc, rest...);
    }
    return true;
}

// ============================================================================
// SAMPLE_PLAY emission (single source of truth)
// ============================================================================
// All SAMPLE_PLAY emission for sample patterns and the scalar sample() builtin
// goes through emit_sample_chain. See docs/prd-sample-emission-unification.md.
struct SamplePatternEmitCtx {
    enum class Kind {
        Pattern,  // pat / chord / velocity / bank / variant / transport-clock
        Scalar,   // builtin sample(trig, pitch, "bd")
    };
    Kind            kind            = Kind::Pattern;
    // Pattern mode: linked SequenceState id. The emitted SAMPLE_PLAY's own
    // state_id is `seq_state_id + 1` so the sampler can read its upstream
    // SEQPAT events without colliding state ids.
    // Scalar mode: caller passes the value compute_state_id() produces from
    // the active semantic-path stack so hot-swap behavior matches the
    // generic-builtin-emission path.
    std::uint32_t   seq_state_id    = 0;
    std::uint16_t   value_buf       = BufferAllocator::BUFFER_UNUSED; // sample-id buffer
    std::uint16_t   trigger_buf     = BufferAllocator::BUFFER_UNUSED;
    // BUFFER_UNUSED → emit no MUL (used by Scalar mode today, which has no
    // velocity input on the sample() builtin).
    std::uint16_t   velocity_buf    = BufferAllocator::BUFFER_UNUSED;
    // BUFFER_UNUSED → helper allocates and emits its own PUSH_CONST 1.0.
    // Scalar callers can pass the user-supplied pitch arg buffer.
    std::uint16_t   pitch_buf       = BufferAllocator::BUFFER_UNUSED;
    // Scalar callers pass builtin->inst_rate to preserve the rate-field
    // bytecode the generic-builtin-emission path sets. Pattern callers leave
    // it at 0 (current pattern path emits SAMPLE_PLAY with rate=0).
    std::uint8_t    rate            = 0;
    SourceLocation  loc             = {};
};

// Free helper used by both class methods (via a thunk that calls this->emit
// to keep source_locations_ in sync) and the static emit_pattern_with_state
// (via a thunk wrapping its existing emit_fn function pointer).
//
// Returns the final audio output buffer, or BufferAllocator::BUFFER_UNUSED on
// allocation failure (caller is responsible for diagnostic emission — both
// existing call sites already short-circuit on this sentinel).
template <typename EmitFn>
inline std::uint16_t emit_sample_chain(
    BufferAllocator& buffers,
    EmitFn&& emit,
    const SamplePatternEmitCtx& ctx) {

    // 1. Pitch input. Scalar callers may already have a pitch buffer; Pattern
    //    callers always allocate a fresh PUSH_CONST 1.0 (sample patterns play
    //    at original pitch — pitch shifting is reserved for sample()).
    std::uint16_t pitch_buf = ctx.pitch_buf;
    if (pitch_buf == BufferAllocator::BUFFER_UNUSED) {
        pitch_buf = buffers.allocate();
        if (pitch_buf == BufferAllocator::BUFFER_UNUSED) {
            return BufferAllocator::BUFFER_UNUSED;
        }
        cedar::Instruction pitch_inst{};
        pitch_inst.opcode = cedar::Opcode::PUSH_CONST;
        pitch_inst.out_buffer = pitch_buf;
        pitch_inst.inputs[0] = 0xFFFF;
        pitch_inst.inputs[1] = 0xFFFF;
        pitch_inst.inputs[2] = 0xFFFF;
        pitch_inst.inputs[3] = 0xFFFF;
        encode_const_value(pitch_inst, 1.0f);
        emit(pitch_inst);
    }

    // 2. SAMPLE_PLAY. Stereo-native (prd-stereo-native-opcodes Phase 3):
    //    allocate an adjacent L/R output pair and set the STEREO_OUTPUT flag
    //    so the opcode writes both channels in one call. inputs[3]/[4] split
    //    the linked SequenceState id in half (Pattern mode) or BUFFER_UNUSED
    //    (Scalar mode, falls back to the scalar sample_id buffer in
    //    op_sample_play).
    std::uint16_t output_buf = buffers.allocate();
    std::uint16_t output_buf_r = buffers.allocate();
    if (output_buf == BufferAllocator::BUFFER_UNUSED ||
        output_buf_r == BufferAllocator::BUFFER_UNUSED ||
        output_buf_r != output_buf + 1) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    cedar::Instruction sample_inst{};
    sample_inst.opcode = cedar::Opcode::SAMPLE_PLAY;
    sample_inst.out_buffer = output_buf;
    sample_inst.inputs[0] = ctx.trigger_buf;
    sample_inst.inputs[1] = pitch_buf;
    sample_inst.inputs[2] = ctx.value_buf;
    sample_inst.rate      = ctx.rate;
    sample_inst.flags     = cedar::InstructionFlag::STEREO_OUTPUT;
    if (ctx.kind == SamplePatternEmitCtx::Kind::Pattern) {
        sample_inst.inputs[3] = static_cast<std::uint16_t>(ctx.seq_state_id & 0xFFFFu);
        sample_inst.inputs[4] = static_cast<std::uint16_t>((ctx.seq_state_id >> 16) & 0xFFFFu);
        sample_inst.state_id  = ctx.seq_state_id + 1;
    } else {
        sample_inst.inputs[3] = 0xFFFF;
        sample_inst.inputs[4] = 0xFFFF;
        sample_inst.state_id  = ctx.seq_state_id;
    }
    emit(sample_inst);

    // 3. Velocity post-multiply. Skipped if velocity_buf is BUFFER_UNUSED
    //    (Scalar mode today). Per-voice velocity is already applied inside
    //    op_sample_play via evt.velocities[v]; the post-MUL exists only to
    //    let `velocity(pat, runtime_expr)` scale the sampler output at
    //    runtime via velocity_buf.
    //
    //    Stereo-native sampler emits a stereo pair (out, out+1). The post-MUL
    //    scales both channels by the same velocity buffer (mono control). The
    //    scaling produces a second stereo pair (scaled, scaled+1).
    if (ctx.velocity_buf == BufferAllocator::BUFFER_UNUSED) {
        return output_buf;
    }
    std::uint16_t scaled_buf = buffers.allocate();
    std::uint16_t scaled_buf_r = buffers.allocate();
    if (scaled_buf == BufferAllocator::BUFFER_UNUSED ||
        scaled_buf_r == BufferAllocator::BUFFER_UNUSED ||
        scaled_buf_r != scaled_buf + 1) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    // Scale L channel
    cedar::Instruction mul_l{};
    mul_l.opcode = cedar::Opcode::MUL;
    mul_l.out_buffer = scaled_buf;
    mul_l.inputs[0] = output_buf;
    mul_l.inputs[1] = ctx.velocity_buf;
    mul_l.inputs[2] = 0xFFFF;
    mul_l.inputs[3] = 0xFFFF;
    mul_l.inputs[4] = 0xFFFF;
    emit(mul_l);
    // Scale R channel
    cedar::Instruction mul_r{};
    mul_r.opcode = cedar::Opcode::MUL;
    mul_r.out_buffer = scaled_buf_r;
    mul_r.inputs[0] = output_buf_r;
    mul_r.inputs[1] = ctx.velocity_buf;
    mul_r.inputs[2] = 0xFFFF;
    mul_r.inputs[3] = 0xFFFF;
    mul_r.inputs[4] = 0xFFFF;
    emit(mul_r);
    return scaled_buf;
}


// ============================================================================
// Pattern Codegen Shared Helpers
// ============================================================================
// Promoted from file-local statics in akkado/src/codegen/patterns.cpp during
// the prd-codegen-sprawl-cleanup Phase 3 file split: these are shared by the
// pattern (patterns.cpp), pattern-transform (pattern_transforms.cpp) and
// pattern-I/O (pattern_io.cpp) TUs.

// Project per-event SequenceSampleMappings to a deduped Pattern-level
// sample_refs vector. One entry per (bank, name, variant) tuple — the global
// `required_samples_extended_` ledger does its own dedup across patterns, but
// per-Pattern dedup keeps the sample_refs surface small for any consumer
// that wants to iterate a single pattern's requirements.
inline std::vector<RequiredSample> sample_refs_from_mappings(
    const std::vector<SequenceSampleMapping>& mappings) {
    std::vector<RequiredSample> refs;
    std::set<std::string> seen;
    refs.reserve(mappings.size());
    for (const auto& m : mappings) {
        RequiredSample r;
        r.bank = m.bank;
        r.name = m.sample_name;
        r.variant = static_cast<int>(m.variant);
        if (seen.insert(r.key()).second) {
            refs.push_back(std::move(r));
        }
    }
    return refs;
}

// Adjust a string-literal token's SourceLocation so it points at the first
// character *inside* the quotes (and spans only the content). This mirrors the
// adjustment the main parser applies in parse_mini_literal(), and must be used
// by every parse_mini() caller so mini-notation node offsets — and therefore
// pattern step-highlighting in the IDE — are consistent across all forms.
inline SourceLocation mini_content_location(const SourceLocation& string_tok) {
    SourceLocation loc = string_tok;
    loc.offset += 1;
    loc.column += 1;
    loc.length = (string_tok.length >= 2) ? string_tok.length - 2 : 0;
    return loc;
}

// Helper: Get pattern argument from a function call
// Returns the MiniLiteral node or NULL_NODE if not a valid pattern
inline NodeIndex get_pattern_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = n.first_child;
    std::size_t idx = 0;
    while (arg != NULL_NODE && idx < arg_index) {
        arg = ast.arena[arg].next_sibling;
        idx++;
    }
    if (arg == NULL_NODE) return NULL_NODE;

    // Unwrap Argument node if present
    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::Argument) {
        arg = arg_node.first_child;
    }

    return arg;
}

// Helper: Get numeric argument from a function call
// Returns the value or default if not a valid number
inline std::optional<float> get_number_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = get_pattern_arg(ast, n, arg_index);
    if (arg == NULL_NODE) return std::nullopt;

    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::NumberLit) {
        return static_cast<float>(arg_node.as_number());
    }
    return std::nullopt;
}

// Helper: Get string argument from a function call
// Returns the string value or nullopt if not a valid string
inline std::optional<std::string> get_string_arg(const Ast& ast, const Node& n, std::size_t arg_index) {
    NodeIndex arg = get_pattern_arg(ast, n, arg_index);
    if (arg == NULL_NODE) return std::nullopt;

    const Node& arg_node = ast.arena[arg];
    if (arg_node.type == NodeType::StringLit) {
        return arg_node.as_string();
    }
    return std::nullopt;
}

// Helper: Check if a Call node calls a known pattern-producing function.
// Does not check MiniLiteral or StringLit (those should be handled separately).
inline bool is_pattern_call(const Node& n, const StringInterner& interner) {
    if (n.type != NodeType::Call) return false;
    std::string func_name = std::string(interner.view(n.as_identifier()));
    return func_name == "timeline" ||
           func_name == "chord" ||
           func_name == "slow" || func_name == "fast" ||
           func_name == "rev" || func_name == "transpose" || func_name == "velocity" ||
           func_name == "bank" || func_name == "variant" || func_name == "transport" ||
           func_name == "tune" ||
           // Phase 2 PRD time/structure transforms
           func_name == "early" || func_name == "late" ||
           func_name == "palindrome" || func_name == "compress" ||
           func_name == "ply" || func_name == "linger" ||
           func_name == "zoom" || func_name == "segment" ||
           func_name == "swing" || func_name == "swingBy" ||
           func_name == "iter" || func_name == "iterBack" ||
           // Phase 2 PRD generators (constructors)
           func_name == "run" || func_name == "binary" || func_name == "binaryN" ||
           // Phase 2 PRD voicing transforms (anchor / mode / voicing only;
           // addVoicings is a registry call, not a pattern producer)
           func_name == "anchor" || func_name == "mode" || func_name == "voicing" ||
           // Phase 2.1 PRD §11.2: standalone note-property transforms
           func_name == "bend" || func_name == "aftertouch" || func_name == "dur";
}

// Helper: Check if a node is a pattern-producing expression.
// Uses symbol table for Identifier nodes (type-based), AST checks for literals/calls.
inline bool is_pattern_node(const Ast& ast, const SymbolTable& symbols, NodeIndex node,
                             const StringInterner& interner) {
    if (node == NULL_NODE) return false;

    const Node& n = ast.arena[node];

    if (n.type == NodeType::MiniLiteral) return true;
    if (n.type == NodeType::StringLit) return true;
    if (is_pattern_call(n, interner)) return true;

    // Identifier: check symbol table for Pattern kind (SymbolId fast path)
    if (n.type == NodeType::Identifier) {
        auto sym = symbols.lookup(n.as_identifier());
        return sym && sym->kind == SymbolKind::Pattern;
    }

    return false;
}

} // namespace codegen
} // namespace akkado
