// Forward control-flow codegen — PRD prd-runtime-functions-control-flow L1.
// Extracted from codegen.cpp for maintainability.
//
// Currently hosts handle_when_call (block-rate conditional bypass). The
// loop()/LOOP_STATIC lowering will join this file in Part B.

#include "akkado/codegen.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/string_interner.hpp"
#include "akkado/codegen/arrays.hpp"
#include "akkado/const_eval.hpp"
#include <cmath>
#include <variant>

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

// Handle loop(count) { body } — bounded static iteration.
//
// The body is threaded: iteration k+1's `@` is iteration k's output, and
// iteration 1's `@` is the loop's seed (the piped-in signal). Lowering:
//
//   <init R from seed>                  -- COPY seed -> R  (once, outside loop)
//   [LOOP_STATIC rate=body_len, out=N]   -- header
//     <body instrs, reading R via @>
//     [COPY body_result -> R]            -- thread the accumulator
//   ; result is R
//
// The VM re-runs [body + COPY] N times. State is shared across iterations
// (the body's stateful opcodes keep one state_id) — see PRD §4.1.
TypedValue CodeGenerator::handle_loop(NodeIndex node, const Node& n) {
    // Children: [count, body, seed?]. seed is appended by the analyzer when
    // the loop is a pipe RHS.
    NodeIndex count_node = n.first_child;
    NodeIndex body_node = (count_node != NULL_NODE)
        ? ast_->arena[count_node].next_sibling : NULL_NODE;
    NodeIndex seed_node = (body_node != NULL_NODE)
        ? ast_->arena[body_node].next_sibling : NULL_NODE;

    if (count_node == NULL_NODE || body_node == NULL_NODE) {
        error("E243", "loop() requires a count and a body", n.location);
        return TypedValue::void_val();
    }

    // --- Constant-fold the iteration count --------------------------------
    ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
    auto count_val = evaluator.evaluate(count_node);
    for (const auto& diag : evaluator.diagnostics()) {
        diagnostics_.push_back(diag);
    }
    if (!count_val || !std::holds_alternative<double>(*count_val)) {
        error("E243", "loop() count must be a compile-time constant integer",
              n.location);
        return TypedValue::void_val();
    }
    double cd = std::get<double>(*count_val);
    if (cd < 0.0 || cd > 65535.0 || cd != std::floor(cd)) {
        error("E243", "loop() count must be a non-negative integer (0-65535)",
              n.location);
        return TypedValue::void_val();
    }
    auto iterations = static_cast<std::uint16_t>(cd);

    // --- Seed: visit the piped-in input, if any ---------------------------
    bool is_stereo = false;
    std::uint16_t seed_l = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t seed_r = 0xFFFF;
    if (seed_node != NULL_NODE) {
        TypedValue seed_tv = visit(seed_node);
        if (seed_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
            error("E243", "loop() input does not produce a signal", n.location);
            return TypedValue::void_val();
        }
        seed_l = seed_tv.buffer;
        seed_r = seed_tv.right_buffer;
        is_stereo = seed_tv.is_stereo() || is_stereo_buffer(seed_l);
        if (is_stereo && seed_r == 0xFFFF) {
            StereoBuffers sb = get_stereo_buffers_by_buffer(seed_l);
            seed_l = sb.left;
            seed_r = sb.right;
        }
    }

    // --- Allocate the running accumulator buffer R ------------------------
    std::uint16_t run_l = buffers_.allocate();
    std::uint16_t run_r = 0xFFFF;
    if (run_l == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    if (is_stereo) {
        run_r = buffers_.allocate();
        if (run_r == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        if (run_r != run_l + 1) {
            error("E166", "Internal error: loop() stereo buffer allocation not adjacent",
                  n.location);
            return TypedValue::void_val();
        }
    }

    // Initialise R: copy the seed in, or zero it when the loop is unpiped.
    if (seed_node != NULL_NODE) {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, run_l, seed_l));
        if (is_stereo) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, run_r, seed_r));
        }
    } else {
        cedar::Instruction zero{};
        zero.opcode = cedar::Opcode::PUSH_CONST;
        zero.out_buffer = run_l;
        codegen::set_unused_inputs(zero);
        codegen::encode_const_value(zero, 0.0f);
        emit(zero);
    }

    // --- LOOP_STATIC header (rate patched after body emission) ------------
    std::size_t loop_idx = instructions_.size();
    {
        cedar::Instruction loop{};
        loop.opcode = cedar::Opcode::LOOP_STATIC;
        loop.out_buffer = iterations;  // iteration count (not a buffer index)
        loop.inputs[0] = 0xFFFF;
        loop.inputs[1] = 0xFFFF;
        loop.inputs[2] = 0xFFFF;
        loop.inputs[3] = 0xFFFF;
        loop.inputs[4] = 0xFFFF;
        loop.rate = 0;  // body_len, patched below
        loop.state_id = 0;
        emit(loop);
    }

    // --- Body: visited once; `@` resolves to the running buffer R ---------
    std::uint32_t count = call_counters_["loop"]++;
    push_path("loop#" + std::to_string(count));

    loop_hole_stack_.push_back(is_stereo
        ? TypedValue::stereo_signal(run_l, run_r)
        : TypedValue::signal(run_l));

    std::size_t body_start = instructions_.size();
    TypedValue body_tv = visit(body_node);
    loop_hole_stack_.pop_back();
    pop_path();

    if (body_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E243", "loop() body must produce a signal", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t body_l = body_tv.buffer;
    std::uint16_t body_r = body_tv.right_buffer;
    bool body_is_stereo = body_tv.is_stereo() || is_stereo_buffer(body_l);
    if (body_is_stereo && body_r == 0xFFFF) {
        StereoBuffers sb = get_stereo_buffers_by_buffer(body_l);
        body_l = sb.left;
        body_r = sb.right;
    }
    if (body_is_stereo != is_stereo) {
        error("E252", "loop() body must produce the same channel count as its "
                      "input (wrap the input in stereo() for a stereo body)",
              n.location);
        return TypedValue::void_val();
    }

    // Thread the body result back into R for the next iteration.
    emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, run_l, body_l));
    if (is_stereo) {
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, run_r, body_r));
    }

    // Patch LOOP_STATIC body length.
    std::size_t body_len = instructions_.size() - body_start;
    if (body_len > 255) {
        error("E253", "loop() body too large (max 255 instructions)", n.location);
        return TypedValue::void_val();
    }
    instructions_[loop_idx].rate = static_cast<std::uint8_t>(body_len);

    if (is_stereo) {
        register_stereo(node, run_l, run_r);
        return cache_and_return(node, TypedValue::stereo_signal(run_l, run_r));
    }
    return cache_and_return(node, TypedValue::signal(run_l));
}

}  // namespace akkado
