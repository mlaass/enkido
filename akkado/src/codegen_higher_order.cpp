// Higher-order DSL codegen — PRD prd-runtime-functions-control-flow L3 §4.3.3.
//
// `each_voice` runs a per-event instrument lambda over a pattern / MIDI event
// stream, mixing every iteration's output. It compiles to a FOREACH_EVENT
// opcode with the PER_ITERATION allocator and a subprogram block body, reusing
// the same subprogram-table machinery the migrated poly() uses.
//
// v1 binding model: the lambda's single parameter is the per-event frequency
// signal (a plain Signal). Full event-record access (`n.freq`, `n.vel`) is a
// documented follow-up — see docs/concepts/higher-order-dsl.md.

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"

namespace akkado {

TypedValue CodeGenerator::handle_each_voice_call(NodeIndex node, const Node& n) {
    using codegen::extract_call_args;

    // each_voice(input, lambda) — method form `pattern.each_voice(lambda)`
    // desugars to the same 2-argument shape.
    auto args = extract_call_args(ast_->arena, n.first_child, 2, 2);
    if (!args.valid) {
        error("E400", "each_voice() requires 2 arguments: each_voice(input, lambda). "
                      "Pipe a pattern in: n\"c4 e4\" |> each_voice((v) -> osc(\"sin\", v))",
              n.location);
        return TypedValue::void_val();
    }

    NodeIndex input_arg = args.nodes[0];
    NodeIndex lambda_arg = args.nodes[1];

    // Visit the event-stream operand. Must resolve to a pattern or a MIDI
    // event source — anything else is E242.
    std::uint32_t seq_state_id = 0;
    {
        auto in_tv = visit(input_arg);
        if (in_tv.pattern) {
            seq_state_id = in_tv.pattern->state_id;
        } else if (in_tv.type == ValueType::EventSource && in_tv.event_source) {
            seq_state_id = in_tv.event_source->state_id;
        } else {
            error("E242", "each_voice/each/fold operand must be a pattern, "
                          "MIDI input, or array literal", n.location);
            return TypedValue::void_val();
        }
        polyphonic_pattern_nodes_.erase(input_arg);
    }

    // Resolve the lambda. v1 requires exactly one parameter (the per-event
    // frequency signal).
    auto func_ref = resolve_function_arg(lambda_arg);
    if (!func_ref) {
        error("E403", "each_voice() second argument must be a function (lambda)",
              n.location);
        return TypedValue::void_val();
    }
    if (func_ref->params.size() != 1) {
        error("E404", "each_voice() lambda must have exactly 1 parameter, got " +
              std::to_string(func_ref->params.size()), n.location);
        return TypedValue::void_val();
    }

    // Convention-slot buffers. The PER_ITERATION executor fills inputs[0] with
    // the per-event value, inputs[1] velocity, inputs[2] duration, and mixes
    // inputs[4] (voice_out, stereo L/R adjacent) into out_buffer (mix L/R).
    std::uint16_t field_buf     = buffers_.allocate();
    std::uint16_t vel_buf       = buffers_.allocate();
    std::uint16_t dur_buf       = buffers_.allocate();
    std::uint16_t voice_out_l   = buffers_.allocate();
    std::uint16_t voice_out_r   = buffers_.allocate();
    std::uint16_t mix_l         = buffers_.allocate();
    std::uint16_t mix_r         = buffers_.allocate();
    if (field_buf == BufferAllocator::BUFFER_UNUSED ||
        vel_buf == BufferAllocator::BUFFER_UNUSED ||
        dur_buf == BufferAllocator::BUFFER_UNUSED ||
        voice_out_l == BufferAllocator::BUFFER_UNUSED ||
        voice_out_r == BufferAllocator::BUFFER_UNUSED ||
        mix_l == BufferAllocator::BUFFER_UNUSED ||
        mix_r == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    if (voice_out_r != voice_out_l + 1 || mix_r != mix_l + 1) {
        error("E166", "Internal error: each_voice stereo buffers not adjacent",
              n.location);
        return TypedValue::void_val();
    }

    std::uint32_t count = call_counters_["each_voice"]++;
    push_path("each_voice#" + std::to_string(count));
    std::uint32_t state_id = compute_state_id();

    // Compile the lambda body into a subprogram block.
    std::uint32_t block_id = begin_subprogram();

    symbols_->push_scope();
    symbols_->define_variable(func_ref->params[0].name, field_buf);
    for (const auto& capture : func_ref->captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }

    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    std::uint16_t body_result = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t body_result_r = 0xFFFF;
    bool body_is_stereo = false;
    {
        TypedValue body_tv = TypedValue::void_val();
        if (func_ref->is_user_function) {
            body_tv = visit(func_ref->closure_node);
        } else {
            const Node& closure_node = ast_->arena[func_ref->closure_node];
            NodeIndex child = closure_node.first_child;
            NodeIndex body = NULL_NODE;
            while (child != NULL_NODE) {
                const Node& child_node = ast_->arena[child];
                if (child_node.type != NodeType::Identifier) { body = child; break; }
                child = ast_->arena[child].next_sibling;
            }
            if (body != NULL_NODE) body_tv = visit(body);
        }
        body_result = body_tv.buffer;
        body_result_r = body_tv.right_buffer;
        body_is_stereo = body_tv.is_stereo() || is_stereo_buffer(body_result);
        if (body_is_stereo && body_result_r == 0xFFFF &&
            body_result != BufferAllocator::BUFFER_UNUSED) {
            StereoBuffers sb = get_stereo_buffers_by_buffer(body_result);
            body_result = sb.left;
            body_result_r = sb.right;
        }
    }

    // Wire the body result into the stereo voice-out pair (mono broadcasts).
    if (body_is_stereo) {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_l, body_result));
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_r, body_result_r));
    } else if (body_result != BufferAllocator::BUFFER_UNUSED) {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_l, body_result));
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_r, body_result));
    }

    for (auto& [k, v] : saved_node_types) {
        if (node_types_.find(k) == node_types_.end()) node_types_[k] = v;
    }
    symbols_->pop_scope();

    std::size_t body_length = subprograms_[block_id].body.size();
    if (body_length > 255) {
        error("E405", "each_voice() lambda body too large (max 255 instructions)",
              n.location);
        end_subprogram(block_id, 3, 2);
        pop_path();
        return TypedValue::void_val();
    }
    end_subprogram(block_id, /*frame_slot_count=*/3, /*output_count=*/2);

    // Emit the FOREACH_EVENT opcode into the main stream.
    cedar::Instruction fe{};
    fe.opcode = cedar::Opcode::FOREACH_EVENT;
    fe.out_buffer = mix_l;
    fe.inputs[0] = field_buf;
    fe.inputs[1] = vel_buf;
    fe.inputs[2] = dur_buf;
    fe.inputs[3] = 0xFFFF;
    fe.inputs[4] = voice_out_l;
    fe.flags = cedar::InstructionFlag::STEREO_OUTPUT;
    fe.rate = 0;
    fe.state_id = state_id;
    emit(fe);

    pop_path();

    // PER_ITERATION FOREACH_EVENT instance config.
    StateInitData fe_init;
    fe_init.state_id = state_id;
    fe_init.type = StateInitData::Type::ForeachAlloc;
    fe_init.foreach_allocator_kind = 1;  // PER_ITERATION
    fe_init.foreach_block_id = block_id;
    fe_init.foreach_event_src_state_id = seq_state_id;
    fe_init.foreach_max_iterations = 64;
    fe_init.foreach_field_slot_count = 3;
    fe_init.foreach_output_count = 2;
    state_inits_.push_back(std::move(fe_init));

    register_stereo(node, mix_l, mix_r);
    return cache_and_return(node, TypedValue::stereo_signal(mix_l, mix_r));
}

}  // namespace akkado
