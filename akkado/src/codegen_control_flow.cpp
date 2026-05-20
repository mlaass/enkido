// Forward control-flow codegen — PRD prd-runtime-functions-control-flow L1.
// Extracted from codegen.cpp for maintainability.
//
// Currently hosts handle_when_call (block-rate conditional bypass). The
// loop()/LOOP_STATIC lowering will join this file in Part B.

#include "akkado/codegen.hpp"
#include "akkado/codegen/arrays.hpp"

namespace akkado {

// Handle when(cond, true_branch, false_branch) — block-rate conditional bypass.
//
// Unlike select(), which always evaluates both branches, when() lowers to
// SKIP_IF_* opcodes so the VM executes only the taken branch's instructions.
// Emitted shape:
//
//   <cond instrs -> cond_buf>
//   [SKIP_IF_ZERO    cond_buf, off1]   ; cond==0 -> jump to false branch
//   <true branch instrs -> true_tv>
//   [COPY true_tv -> res_buf]          ; (2x COPY when stereo)
//   [SKIP_IF_NONZERO cond_buf, off2]   ; cond!=0 -> jump past false branch
//   <false branch instrs -> false_tv>
//   [COPY false_tv -> res_buf]         ; (2x COPY when stereo)
//   ; merge point — result is res_buf
//
// Both skips test the same cond_buf, so no dedicated zero-buffer is needed:
// when cond==0 the first skip jumps over the true branch *and* the
// SKIP_IF_NONZERO, landing on the false branch; when cond!=0 the true branch
// runs and SKIP_IF_NONZERO jumps over the false branch.
TypedValue CodeGenerator::handle_when_call(NodeIndex node, const Node& n) {
    using codegen::extract_call_args;

    auto args = extract_call_args(ast_->arena, n.first_child, 3, 3);
    if (!args.valid) {
        error("E400",
              "when() requires exactly 3 arguments: when(cond, true_branch, false_branch)",
              n.location);
        return TypedValue::void_val();
    }

    // Unique semantic-path component so stateful UGens inside the branches get
    // distinct state IDs across when() call sites and across the two branches.
    std::uint32_t count = call_counters_["when"]++;
    push_path("when#" + std::to_string(count));

    // --- Condition ---------------------------------------------------------
    TypedValue cond_tv = visit(args.nodes[0]);
    if (cond_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E247", "when() condition must produce a signal", n.location);
        pop_path();
        return TypedValue::void_val();
    }
    std::uint16_t cond_buf = cond_tv.buffer;

    // --- SKIP_IF_ZERO (skip true branch) — emitted with placeholder offset --
    std::size_t skip1_idx = instructions_.size();
    {
        cedar::Instruction skip{};
        skip.opcode = cedar::Opcode::SKIP_IF_ZERO;
        skip.out_buffer = 0xFFFF;
        skip.inputs[0] = cond_buf;
        skip.inputs[1] = 0xFFFF;
        skip.inputs[2] = 0xFFFF;
        skip.inputs[3] = 0xFFFF;
        skip.inputs[4] = 0xFFFF;
        skip.rate = 0;  // patched below
        skip.state_id = 0;
        emit(skip);
    }

    // --- True branch -------------------------------------------------------
    push_path("true");
    TypedValue true_tv = visit(args.nodes[1]);
    pop_path();

    if (true_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E247", "when() true branch must produce a signal", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Resolve the true branch's channel layout. The TypedValue may not carry
    // the R buffer for alias/variable lookups — fall back to the stereo map.
    std::uint16_t true_l = true_tv.buffer;
    std::uint16_t true_r = true_tv.right_buffer;
    bool true_is_stereo = true_tv.is_stereo() || is_stereo_buffer(true_l);
    if (true_is_stereo && true_r == 0xFFFF) {
        StereoBuffers sb = get_stereo_buffers_by_buffer(true_l);
        true_l = sb.left;
        true_r = sb.right;
    }

    // Allocate the merge buffer(s) now that the channel layout is known.
    std::uint16_t res_l = buffers_.allocate();
    std::uint16_t res_r = 0xFFFF;
    if (res_l == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }
    if (true_is_stereo) {
        res_r = buffers_.allocate();
        if (res_r == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        if (res_r != res_l + 1) {
            error("E166", "Internal error: when() stereo buffer allocation not adjacent",
                  n.location);
            pop_path();
            return TypedValue::void_val();
        }
    }

    // Copy the true branch result into the merge buffer. COPY is mono-only, so
    // a stereo branch needs two COPYs.
    if (true_is_stereo) {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, res_l, true_l));
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, res_r, true_r));
    } else {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, res_l, true_l));
    }

    // --- SKIP_IF_NONZERO (skip false branch) — placeholder offset ----------
    std::size_t skip2_idx = instructions_.size();
    {
        cedar::Instruction skip{};
        skip.opcode = cedar::Opcode::SKIP_IF_NONZERO;
        skip.out_buffer = 0xFFFF;
        skip.inputs[0] = cond_buf;
        skip.inputs[1] = 0xFFFF;
        skip.inputs[2] = 0xFFFF;
        skip.inputs[3] = 0xFFFF;
        skip.inputs[4] = 0xFFFF;
        skip.rate = 0;  // patched below
        skip.state_id = 0;
        emit(skip);
    }

    // --- False branch ------------------------------------------------------
    push_path("false");
    TypedValue false_tv = visit(args.nodes[2]);
    pop_path();

    if (false_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E247", "when() false branch must produce a signal", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    std::uint16_t false_l = false_tv.buffer;
    std::uint16_t false_r = false_tv.right_buffer;
    bool false_is_stereo = false_tv.is_stereo() || is_stereo_buffer(false_l);
    if (false_is_stereo && false_r == 0xFFFF) {
        StereoBuffers sb = get_stereo_buffers_by_buffer(false_l);
        false_l = sb.left;
        false_r = sb.right;
    }

    if (false_is_stereo != true_is_stereo) {
        error("E247", "when() branches must have matching output channel count",
              n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Copy the false branch result into the same merge buffer.
    if (false_is_stereo) {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, res_l, false_l));
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, res_r, false_r));
    } else {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, res_l, false_l));
    }

    // --- Patch skip offsets ------------------------------------------------
    // off1: when cond==0, ip = skip1_idx + off1 + 1 must land on the first
    //       false-branch instruction (skip2_idx + 1) -> off1 = skip2_idx - skip1_idx.
    // off2: when cond!=0, ip = skip2_idx + off2 + 1 must land on the merge
    //       point -> off2 = merge_idx - skip2_idx - 1.
    std::size_t merge_idx = instructions_.size();
    std::size_t off1 = skip2_idx - skip1_idx;
    std::size_t off2 = merge_idx - skip2_idx - 1;
    if (off1 > 255 || off2 > 255) {
        error("E248", "when() branch too large (max 255 instructions per branch)",
              n.location);
        pop_path();
        return TypedValue::void_val();
    }
    instructions_[skip1_idx].rate = static_cast<std::uint8_t>(off1);
    instructions_[skip2_idx].rate = static_cast<std::uint8_t>(off2);

    pop_path();  // "when#N"

    if (true_is_stereo) {
        register_stereo(node, res_l, res_r);
        return cache_and_return(node, TypedValue::stereo_signal(res_l, res_r));
    }
    return cache_and_return(node, TypedValue::signal(res_l));
}

}  // namespace akkado
