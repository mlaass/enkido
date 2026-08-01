// Stereo signal codegen implementations
// Handles stereo(), left(), right(), pan(), width(), ms_encode(), ms_decode(), pingpong()

#include "akkado/codegen.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/string_interner.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/instruction_builder.hpp"
#include <algorithm>

namespace akkado {

using codegen::unwrap_argument;
using codegen::extract_call_args;

// ============================================================================
// Stereo tracking
// ============================================================================

void CodeGenerator::register_stereo(NodeIndex node, std::uint16_t left, std::uint16_t right) {
    stereo_outputs_[node] = {left, right};
    // Also register by buffer index for variable propagation
    stereo_buffer_pairs_[left] = right;
    // If node_types_ already has an entry, patch in the stereo fields so
    // downstream consumers (e.g. auto-lift) can read channels off TypedValue.
    auto it = node_types_.find(node);
    if (it != node_types_.end()) {
        it->second.channels = ChannelCount::Stereo;
        it->second.right_buffer = right;
    }
}

bool CodeGenerator::is_stereo(NodeIndex node) const {
    // First check node-based tracking
    if (stereo_outputs_.find(node) != stereo_outputs_.end()) {
        return true;
    }
    // Fallback: check if the node's buffer is a stereo left channel
    auto buf_it = node_types_.find(node);
    if (buf_it != node_types_.end()) {
        return stereo_buffer_pairs_.find(buf_it->second.buffer) != stereo_buffer_pairs_.end();
    }
    return false;
}

bool CodeGenerator::is_stereo_buffer(std::uint16_t buffer) const {
    return stereo_buffer_pairs_.find(buffer) != stereo_buffer_pairs_.end();
}

CodeGenerator::StereoBuffers CodeGenerator::get_stereo_buffers(NodeIndex node) const {
    // First try node-based tracking
    auto it = stereo_outputs_.find(node);
    if (it != stereo_outputs_.end()) {
        return it->second;
    }
    // Fallback: check buffer-based tracking
    auto buf_it = node_types_.find(node);
    if (buf_it != node_types_.end()) {
        auto pair_it = stereo_buffer_pairs_.find(buf_it->second.buffer);
        if (pair_it != stereo_buffer_pairs_.end()) {
            return {buf_it->second.buffer, pair_it->second};
        }
    }
    // Not stereo - return same buffer for both (mono)
    if (buf_it != node_types_.end()) {
        return {buf_it->second.buffer, buf_it->second.buffer};
    }
    return {BufferAllocator::BUFFER_UNUSED, BufferAllocator::BUFFER_UNUSED};
}

CodeGenerator::StereoBuffers CodeGenerator::get_stereo_buffers_by_buffer(std::uint16_t buffer) const {
    auto pair_it = stereo_buffer_pairs_.find(buffer);
    if (pair_it != stereo_buffer_pairs_.end()) {
        return {buffer, pair_it->second};
    }
    // Not stereo - return same buffer for both (mono)
    return {buffer, buffer};
}

// ============================================================================
// Stereo handlers
// ============================================================================

// mono(stereo) -> sum-to-mono downmix (L+R)*0.5
// Dispatches to the poly()-style voice-manager form mono(instrument) /
// mono(input, instrument) when ANY argument resolves to a function reference,
// preserving backward compatibility with the existing mono voice manager
// (which accepts `mono(fn)`, `mono(fn, pattern)`, or `mono(pattern, fn)`).
TypedValue CodeGenerator::handle_mono_call(NodeIndex node, const Node& n) {
    // Any function-valued argument → route to the voice manager.
    for (NodeIndex arg = n.first_child; arg != NULL_NODE;
         arg = ast_->arena[arg].next_sibling) {
        NodeIndex unwrapped = unwrap_argument(ast_->arena, arg);
        if (resolve_function_arg(unwrapped).has_value()) {
            return handle_poly_call(node, n);
        }
    }

    auto args = extract_call_args(ast_->arena, n.first_child, 1, 1);
    if (!args.valid || args.nodes.empty()) {
        error("E180", "mono() requires 1 stereo-signal argument", n.location);
        return TypedValue::error_val();
    }

    NodeIndex signal_node = args.nodes[0];
    TypedValue input = visit(signal_node);

    // Resolve stereo via TypedValue.channels first, fall back to legacy maps
    // for signals routed through variables before the type system reached them.
    bool input_is_stereo = input.is_stereo()
                           || is_stereo(signal_node)
                           || is_stereo_buffer(input.buffer);
    if (!input_is_stereo) {
        // PRD prd-stereo-native-opcodes §5.6: mono(mono) is a silent no-op
        // returning the input unchanged. Warning surfaces the redundant call
        // at the source location.
        warn("W181", "mono() called on a mono signal — returning the input unchanged.",
             n.location);
        return cache_and_return(node, input);
    }

    std::uint16_t left_buf = input.buffer;
    std::uint16_t right_buf = input.right_buffer;
    if (right_buf == 0xFFFF) {
        // Variable/alias path — look up via legacy map
        StereoBuffers sb = is_stereo(signal_node)
            ? get_stereo_buffers(signal_node)
            : get_stereo_buffers_by_buffer(left_buf);
        left_buf = sb.left;
        right_buf = sb.right;
    }

    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::MONO_DOWNMIX)
            .input(0, left_buf)
            .input(1, right_buf)
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::error_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

// stereo(mono) -> duplicate mono to both L and R
// stereo(left, right) -> create stereo pair from two signals
TypedValue CodeGenerator::handle_stereo_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);

    if (!args.valid || args.nodes.empty()) {
        error("E160", "stereo() requires 1 or 2 arguments", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t left_buf, right_buf;

    if (args.nodes.size() == 1) {
        // stereo(mono) - check if input is already stereo
        NodeIndex mono_node = args.nodes[0];
        TypedValue mono_tv = visit(mono_node);
        std::uint16_t mono_buf = mono_tv.buffer;

        // Check stereo by both node and buffer (buffer fallback for pipe chains)
        bool input_is_stereo = mono_tv.is_stereo()
                               || is_stereo(mono_node)
                               || is_stereo_buffer(mono_buf);

        if (input_is_stereo) {
            // PRD prd-stereo-native-opcodes §5.6: stereo(stereo) is a silent
            // no-op returning the input unchanged. Warning surfaces the
            // redundant call. Resolve full L/R via legacy maps if the
            // TypedValue itself doesn't carry them (variable/alias path).
            warn("W182",
                 "stereo() called on a stereo signal — returning the input unchanged.",
                 n.location);
            if (mono_tv.right_buffer != 0xFFFF) {
                register_stereo(node, mono_tv.buffer, mono_tv.right_buffer);
                return cache_and_return(node, mono_tv);
            }
            StereoBuffers sb = is_stereo(mono_node)
                ? get_stereo_buffers(mono_node)
                : get_stereo_buffers_by_buffer(mono_buf);
            register_stereo(node, sb.left, sb.right);
            return cache_and_return(node,
                TypedValue::stereo_signal(sb.left, sb.right));
        }

        // Mono input - allocate two new buffers and copy
        left_buf = buffers_.allocate();
        right_buf = buffers_.allocate();

        if (left_buf == BufferAllocator::BUFFER_UNUSED ||
            right_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }

        // Emit COPY instructions to duplicate mono to both channels
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, left_buf, mono_buf));
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, right_buf, mono_buf));
    } else {
        // stereo(left, right) - use provided buffers directly
        left_buf = visit(args.nodes[0]).buffer;
        right_buf = visit(args.nodes[1]).buffer;
    }

    register_stereo(node, left_buf, right_buf);
    return cache_and_return(node, TypedValue::stereo_signal(left_buf, right_buf));
}

// left(stereo) -> extract left channel
TypedValue CodeGenerator::handle_left_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 1);

    if (!args.valid || args.nodes.empty()) {
        error("E161", "left() requires 1 argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex stereo_node = args.nodes[0];
    std::uint16_t buf = visit(stereo_node).buffer;

    // Check stereo by both node and buffer (buffer fallback for pipe chains)
    bool input_is_stereo = is_stereo(stereo_node) || is_stereo_buffer(buf);

    if (input_is_stereo) {
        StereoBuffers stereo;
        if (is_stereo(stereo_node)) {
            stereo = get_stereo_buffers(stereo_node);
        } else {
            stereo = get_stereo_buffers_by_buffer(buf);
        }
        return cache_and_return(node, TypedValue::signal(stereo.left));
    }

    // PRD prd-stereo-native-opcodes §5.6: left(mono) returns the input
    // unchanged with W183 — "the channel being extracted is the only one
    // that exists." Surfaces the redundant call at source location.
    warn("W183", "left() called on a mono signal — returning the input unchanged.",
         n.location);
    return cache_and_return(node, TypedValue::signal(buf));
}

// right(stereo) -> extract right channel
TypedValue CodeGenerator::handle_right_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 1);

    if (!args.valid || args.nodes.empty()) {
        error("E163", "right() requires 1 argument", n.location);
        return TypedValue::void_val();
    }

    NodeIndex stereo_node = args.nodes[0];
    std::uint16_t buf = visit(stereo_node).buffer;

    // Check stereo by both node and buffer (buffer fallback for pipe chains)
    bool input_is_stereo = is_stereo(stereo_node) || is_stereo_buffer(buf);

    if (input_is_stereo) {
        StereoBuffers stereo;
        if (is_stereo(stereo_node)) {
            stereo = get_stereo_buffers(stereo_node);
        } else {
            stereo = get_stereo_buffers_by_buffer(buf);
        }
        return cache_and_return(node, TypedValue::signal(stereo.right));
    }

    // PRD prd-stereo-native-opcodes §5.6: right(mono) returns the input
    // unchanged with W184. See left() for the symmetric rationale.
    warn("W184", "right() called on a mono signal — returning the input unchanged.",
         n.location);
    return cache_and_return(node, TypedValue::signal(buf));
}

// pan(mono, pos) -> equal-power mono → stereo pan (PAN opcode)
// pan(stereo, pos) -> equal-power stereo balance (PAN_STEREO opcode, per PRD §5.5)
TypedValue CodeGenerator::handle_pan_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2, 2);

    if (!args.valid || args.nodes.size() < 2) {
        error("E165", "pan() requires 2 arguments: pan(signal, position)", n.location);
        return TypedValue::void_val();
    }

    TypedValue src = visit(args.nodes[0]);
    std::uint16_t pos_buf = visit(args.nodes[1]).buffer;

    bool src_is_stereo = src.is_stereo()
                         || is_stereo(args.nodes[0])
                         || is_stereo_buffer(src.buffer);

    // Resolve L/R buffers for the stereo-balance overload
    std::uint16_t src_left = src.buffer;
    std::uint16_t src_right = src.right_buffer;
    if (src_is_stereo && src_right == 0xFFFF) {
        StereoBuffers sb = is_stereo(args.nodes[0])
            ? get_stereo_buffers(args.nodes[0])
            : get_stereo_buffers_by_buffer(src_left);
        src_left = sb.left;
        src_right = sb.right;
    }

    // Allocate two adjacent output buffers for stereo
    std::uint16_t left_buf = buffers_.allocate();
    std::uint16_t right_buf = buffers_.allocate();

    if (left_buf == BufferAllocator::BUFFER_UNUSED ||
        right_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Ensure right_buf = left_buf + 1 (required by PAN/PAN_STEREO opcodes)
    if (right_buf != left_buf + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return TypedValue::void_val();
    }

    codegen::InstructionBuilder builder(
        src_is_stereo ? cedar::Opcode::PAN_STEREO : cedar::Opcode::PAN);
    if (src_is_stereo) {
        builder.inputs({src_left, src_right, pos_buf});
    } else {
        builder.inputs({src.buffer, pos_buf});
    }
    // L output; R is implicitly left_buf + 1
    builder.output(left_buf).emit(*this);

    register_stereo(node, left_buf, right_buf);
    return cache_and_return(node, TypedValue::stereo_signal(left_buf, right_buf));
}

// width(stereo, amount) or width(L, R, amount) -> adjust stereo width
TypedValue CodeGenerator::handle_width_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2, 3);

    if (!args.valid || args.nodes.size() < 2) {
        error("E168", "width() requires 2 or 3 arguments", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t left_buf, right_buf, width_buf;

    if (args.nodes.size() == 2) {
        // width(stereo, amount) - convenience form
        NodeIndex stereo_node = args.nodes[0];
        visit(stereo_node);
        width_buf = visit(args.nodes[1]).buffer;

        if (is_stereo(stereo_node)) {
            auto stereo = get_stereo_buffers(stereo_node);
            left_buf = stereo.left;
            right_buf = stereo.right;
        } else {
            // Mono input - treat as stereo with same signal on both channels
            auto it = node_types_.find(stereo_node);
            if (it == node_types_.end()) {
                error("E167", "width() first argument is not a signal", n.location);
                return TypedValue::void_val();
            }
            left_buf = it->second.buffer;
            right_buf = it->second.buffer;
        }
    } else {
        // width(L, R, amount) - explicit form
        left_buf = visit(args.nodes[0]).buffer;
        right_buf = visit(args.nodes[1]).buffer;
        width_buf = visit(args.nodes[2]).buffer;
    }

    // Allocate output buffers
    std::uint16_t out_left = buffers_.allocate();
    std::uint16_t out_right = buffers_.allocate();

    if (out_left == BufferAllocator::BUFFER_UNUSED ||
        out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return TypedValue::void_val();
    }

    codegen::InstructionBuilder(cedar::Opcode::WIDTH)
        .inputs({left_buf, right_buf, width_buf})
        .output(out_left)
        .emit(*this);

    register_stereo(node, out_left, out_right);
    return cache_and_return(node, TypedValue::stereo_signal(out_left, out_right));
}

// ms_encode(stereo) or ms_encode(L, R) -> convert to mid/side
TypedValue CodeGenerator::handle_ms_encode_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);

    if (!args.valid || args.nodes.empty()) {
        error("E170", "ms_encode() requires 1 or 2 arguments", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t left_buf, right_buf;

    if (args.nodes.size() == 1) {
        // ms_encode(stereo) - convenience form
        NodeIndex stereo_node = args.nodes[0];
        visit(stereo_node);

        if (is_stereo(stereo_node)) {
            auto stereo = get_stereo_buffers(stereo_node);
            left_buf = stereo.left;
            right_buf = stereo.right;
        } else {
            // Mono - mid = signal, side = 0
            auto it = node_types_.find(stereo_node);
            if (it == node_types_.end()) {
                error("E169", "ms_encode() argument is not a signal", n.location);
                return TypedValue::void_val();
            }
            left_buf = it->second.buffer;
            right_buf = it->second.buffer;
        }
    } else {
        // ms_encode(L, R) - explicit form
        left_buf = visit(args.nodes[0]).buffer;
        right_buf = visit(args.nodes[1]).buffer;
    }

    // Allocate output buffers (mid, side)
    std::uint16_t out_mid = buffers_.allocate();
    std::uint16_t out_side = buffers_.allocate();

    if (out_mid == BufferAllocator::BUFFER_UNUSED ||
        out_side == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    if (out_side != out_mid + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return TypedValue::void_val();
    }

    codegen::InstructionBuilder(cedar::Opcode::MS_ENCODE)
        .inputs({left_buf, right_buf})
        .output(out_mid)
        .emit(*this);

    // Register as stereo-like (mid/side pair)
    register_stereo(node, out_mid, out_side);
    return cache_and_return(node, TypedValue::stereo_signal(out_mid, out_side));
}

// ms_decode(ms) or ms_decode(M, S) -> convert mid/side to stereo
TypedValue CodeGenerator::handle_ms_decode_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);

    if (!args.valid || args.nodes.empty()) {
        error("E172", "ms_decode() requires 1 or 2 arguments", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t mid_buf, side_buf;

    if (args.nodes.size() == 1) {
        // ms_decode(ms) - convenience form (input is stereo-like mid/side pair)
        NodeIndex ms_node = args.nodes[0];
        visit(ms_node);

        if (is_stereo(ms_node)) {
            auto stereo = get_stereo_buffers(ms_node);
            mid_buf = stereo.left;   // Treat as M
            side_buf = stereo.right;  // Treat as S
        } else {
            // Mono - just return as-is (mid only, no side info)
            auto it = node_types_.find(ms_node);
            if (it == node_types_.end()) {
                error("E171", "ms_decode() argument is not a signal", n.location);
                return TypedValue::void_val();
            }
            mid_buf = it->second.buffer;
            // Allocate a zero buffer for side
            side_buf = codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                           .const_value(0.0f)
                           .emit(*this, n.location);
            if (side_buf == BufferAllocator::BUFFER_UNUSED) {
                return TypedValue::void_val();
            }
        }
    } else {
        // ms_decode(M, S) - explicit form
        mid_buf = visit(args.nodes[0]).buffer;
        side_buf = visit(args.nodes[1]).buffer;
    }

    // Allocate output buffers (left, right)
    std::uint16_t out_left = buffers_.allocate();
    std::uint16_t out_right = buffers_.allocate();

    if (out_left == BufferAllocator::BUFFER_UNUSED ||
        out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return TypedValue::void_val();
    }

    codegen::InstructionBuilder(cedar::Opcode::MS_DECODE)
        .inputs({mid_buf, side_buf})
        .output(out_left)
        .emit(*this);

    register_stereo(node, out_left, out_right);
    return cache_and_return(node, TypedValue::stereo_signal(out_left, out_right));
}

// pingpong(stereo, time, fb [, width [, dry, wet]])
// pingpong(L, R, time, fb [, width [, dry, wet]])
// dry/wet are kwargs (find_param("dry") = 5, find_param("wet") = 6) routed
// through ExtendedParams<2>; the analyzer reorders kwargs and inserts `_`
// placeholders for any gaps, which we resolve to PUSH_CONST defaults below.
TypedValue CodeGenerator::handle_pingpong_call(NodeIndex node, const Node& n) {
    // Up to 7 children after analyzer reorder: 5 positional slots + dry + wet.
    auto args = extract_call_args(ast_->arena, n.first_child, 3, 7);

    if (!args.valid || args.nodes.size() < 3) {
        error("E175", "pingpong() requires at least 3 arguments", n.location);
        return TypedValue::void_val();
    }

    auto is_placeholder = [&](NodeIndex idx) -> bool {
        if (idx == NULL_NODE) return true;
        const Node& v = ast_->arena[idx];
        return v.type == NodeType::Identifier &&
               std::holds_alternative<Node::IdentifierData>(v.data) &&
               ctx_->interner->view(v.as_identifier()) == "_";
    };

    // Resolve a dry/wet extended-param slot: missing or `_` → leave as
    // BUFFER_UNUSED so emit_extended_params_init() inlines the constant
    // default. Otherwise visit the supplied arg.
    auto resolve_ext = [&](std::size_t idx) -> std::uint16_t {
        if (idx >= args.nodes.size() || is_placeholder(args.nodes[idx])) {
            return BufferAllocator::BUFFER_UNUSED;
        }
        return visit(args.nodes[idx]).buffer;
    };

    std::uint16_t left_buf, right_buf, time_buf, fb_buf;
    std::uint16_t width_buf = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t dry_buf, wet_buf;

    // Check first argument to determine form
    NodeIndex first_node = args.nodes[0];
    std::uint16_t first_buf = visit(first_node).buffer;
    bool first_is_stereo = is_stereo(first_node) || is_stereo_buffer(first_buf);

    if (first_is_stereo) {
        // pingpong(stereo, time, fb, width?, _?, dry?, wet?)
        // After kwargs reorder both width (pos 3) and the unused pos-4 slot may
        // be `_` placeholders — pos 4 is the explicit form's width and is
        // ignored here, so we just consume the buffer for arg_buffers symmetry.
        StereoBuffers stereo;
        if (is_stereo(first_node)) {
            stereo = get_stereo_buffers(first_node);
        } else {
            stereo = get_stereo_buffers_by_buffer(first_buf);
        }
        left_buf = stereo.left;
        right_buf = stereo.right;

        time_buf = visit(args.nodes[1]).buffer;
        fb_buf = visit(args.nodes[2]).buffer;

        // Stereo-form width sits at positional slot 3 (param "fb?" in the
        // overloaded names array maps here for stereo callers).
        if (args.nodes.size() >= 4 && !is_placeholder(args.nodes[3])) {
            width_buf = visit(args.nodes[3]).buffer;
        }
    } else {
        // pingpong(L, R, time, fb, width?, dry?, wet?)
        if (args.nodes.size() < 4 || is_placeholder(args.nodes[3])) {
            error("E173", "pingpong() requires at least 4 arguments: pingpong(L, R, time, fb)", n.location);
            return TypedValue::void_val();
        }

        auto it = node_types_.find(first_node);
        if (it == node_types_.end()) {
            error("E174", "pingpong() first argument is not a signal", n.location);
            return TypedValue::void_val();
        }
        left_buf = it->second.buffer;
        right_buf = visit(args.nodes[1]).buffer;
        time_buf = visit(args.nodes[2]).buffer;
        fb_buf = visit(args.nodes[3]).buffer;

        // Explicit-form width sits at positional slot 4.
        if (args.nodes.size() >= 5 && !is_placeholder(args.nodes[4])) {
            width_buf = visit(args.nodes[4]).buffer;
        }
    }

    // Extended-param slots: dry at position 5, wet at position 6 (set by the
    // analyzer via find_param on extended_param_names).
    dry_buf = resolve_ext(5);
    wet_buf = resolve_ext(6);

    // Allocate output buffers
    std::uint16_t out_left = buffers_.allocate();
    std::uint16_t out_right = buffers_.allocate();

    if (out_left == BufferAllocator::BUFFER_UNUSED ||
        out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return TypedValue::void_val();
    }

    // Generate state_id for the stateful delay
    std::uint32_t count = call_counters_["pingpong"]++;
    push_path("pingpong#" + std::to_string(count));
    std::uint32_t state_id = compute_state_id();
    pop_path();

    // width_buf is BUFFER_UNUSED if not provided
    codegen::InstructionBuilder(cedar::Opcode::DELAY_PINGPONG)
        .inputs({left_buf, right_buf, time_buf, fb_buf, width_buf})
        .state_id(state_id)
        .output(out_left)
        .emit(*this);

    // Emit ExtendedParams<2> for dry/wet. arg_buffers needs entries 0..6 so
    // emit_extended_params_init() can read [total_params()+i] = [5+i]; the
    // earlier slots aren't read by the helper but we populate them for clarity.
    const BuiltinInfo* info = lookup_builtin("pingpong");
    if (info && info->extended_param_count > 0) {
        std::vector<std::uint16_t> arg_buffers = {
            left_buf, right_buf, time_buf, fb_buf, width_buf, dry_buf, wet_buf
        };
        emit_extended_params_init(state_id, *info, arg_buffers);
    }

    register_stereo(node, out_left, out_right);
    return cache_and_return(node, TypedValue::stereo_signal(out_left, out_right));
}

}  // namespace akkado
