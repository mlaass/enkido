#include "cedar/vm/vm.hpp"
#include "cedar/io/midi_sequence.hpp"
#include "cedar/opcodes/opcodes.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <utility>

namespace cedar {

VM::VM() {
    // Initialize context with pointers to our pools
    ctx_.buffers = &buffer_pool_;
    ctx_.states = &state_pool_;
    ctx_.arena = &audio_arena_;
    ctx_.env_map = &env_map_;

    // Clear BUFFER_ZERO - this reserved buffer is always 0.0
    // Used as default for optional inputs (phase, trigger, etc.)
    buffer_pool_.clear(BUFFER_ZERO);
}

VM::~VM() = default;

// ============================================================================
// Program Loading
// ============================================================================

void VM::set_block_table(std::span<const BlockEntry> block_table,
                         std::uint32_t main_inst_count) {
    pending_block_count_ = static_cast<std::uint32_t>(
        std::min<std::size_t>(block_table.size(), MAX_SUBPROGRAMS));
    std::copy_n(block_table.begin(), pending_block_count_,
                pending_blocks_.begin());
    pending_main_count_ = main_inst_count;
    has_pending_blocks_ = true;
}

VM::LoadResult VM::load_program(std::span<const Instruction> bytecode) {
    if (bytecode.size() > MAX_PROGRAM_SIZE) {
        has_pending_blocks_ = false;
        return LoadResult::TooLarge;
    }

    bool ok;
    if (has_pending_blocks_) {
        ok = swap_controller_.load_program(
            bytecode,
            std::span<const BlockEntry>(pending_blocks_.data(),
                                        pending_block_count_),
            pending_main_count_);
        has_pending_blocks_ = false;
    } else {
        ok = swap_controller_.load_program(bytecode);
    }

    if (!ok) {
        return LoadResult::SlotBusy;
    }

    return LoadResult::Success;
}

bool VM::load_program_immediate(std::span<const Instruction> bytecode) {
    // Reset everything first
    reset();

    // Load directly into current slot
    ProgramSlot* slot = swap_controller_.acquire_write_slot();
    if (!slot) { has_pending_blocks_ = false; return false; }

    bool ok;
    if (has_pending_blocks_) {
        ok = slot->load(bytecode,
                        std::span<const BlockEntry>(pending_blocks_.data(),
                                                    pending_block_count_),
                        pending_main_count_);
        has_pending_blocks_ = false;
    } else {
        ok = slot->load(bytecode);
    }
    if (!ok) {
        slot->state.store(ProgramSlot::State::Empty, std::memory_order_release);
        return false;
    }

    // Submit and immediately swap
    swap_controller_.submit_ready(slot);
    swap_controller_.execute_swap();

    return true;
}

VM::LoadResult VM::load_program_with_blocks(
    std::span<const Instruction> bytecode,
    std::span<const BlockEntry> block_table,
    std::uint32_t main_inst_count) {
    if (block_table.size() > MAX_SUBPROGRAMS) {
        return LoadResult::InvalidProgram;
    }
    set_block_table(block_table, main_inst_count);
    return load_program(bytecode);
}

bool VM::load_program_with_blocks_immediate(
    std::span<const Instruction> bytecode,
    std::span<const BlockEntry> block_table,
    std::uint32_t main_inst_count) {
    if (block_table.size() > MAX_SUBPROGRAMS) {
        return false;
    }
    set_block_table(block_table, main_inst_count);
    return load_program_immediate(bytecode);
}

// ============================================================================
// Audio Processing
// ============================================================================

void VM::process_block(float* output_left, float* output_right) {
    // Clear output buffers
    std::fill_n(output_left, BLOCK_SIZE, 0.0f);
    std::fill_n(output_right, BLOCK_SIZE, 0.0f);

    // Handle swap at block boundary
    handle_swap();

    // Get current program slot
    const ProgramSlot* current = swap_controller_.current_slot();
    if (!current || current->instruction_count == 0) {
        // Don't advance clock when no program is loaded.
        // This ensures the first program starts from beat 0.
        return;
    }

    // Update timing
    ctx_.update_timing();

#ifndef CEDAR_NO_FFT
    // Snapshot all registered wavetable banks for the duration of this
    // block. wavetable_pins_ pins shared_ptrs (allocation-free — just
    // refcount bumps); ctx_.wavetable_banks gets raw pointers indexable
    // by inst.rate.
    wavetable_registry_.snapshot(ctx_.wavetable_banks, wavetable_pins_);
#endif

    // Check if crossfading
    if (crossfade_state_.is_active()) {
        perform_crossfade(output_left, output_right);
    } else {
        // Normal execution
        execute_program(current, output_left, output_right);
    }

    // Advance timing
    ctx_.global_sample_counter += BLOCK_SIZE;
    ctx_.block_counter++;
}

void VM::handle_swap() {
    // Handle crossfade completion
    if (crossfade_state_.is_completing()) {
        std::fprintf(stderr, "[VM] Crossfade completing - BEFORE release: has_program=%d swap_count=%u\n",
                    swap_controller_.has_program() ? 1 : 0, swap_controller_.swap_count());
        swap_controller_.release_previous();
        std::fprintf(stderr, "[VM] AFTER release_previous: has_program=%d swap_count=%u\n",
                    swap_controller_.has_program() ? 1 : 0, swap_controller_.swap_count());
        crossfade_state_.complete();
        std::fprintf(stderr, "[VM] AFTER complete: has_program=%d swap_count=%u\n",
                    swap_controller_.has_program() ? 1 : 0, swap_controller_.swap_count());
        // Move orphaned states to fading pool
        state_pool_.gc_sweep();
        std::fprintf(stderr, "[VM] AFTER gc_sweep: has_program=%d swap_count=%u\n",
                    swap_controller_.has_program() ? 1 : 0, swap_controller_.swap_count());
    }

    // Advance fade-out for orphaned states (every block)
    state_pool_.advance_fading();
    state_pool_.gc_fading();

    // Advance crossfade if active
    if (crossfade_state_.is_active()) {
        crossfade_state_.advance();
        return;  // Already crossfading, don't start another
    }

    // Check for pending swap
    if (!swap_controller_.has_pending_swap()) {
        return;
    }

    std::fprintf(stderr, "[VM] handle_swap: pending=1, crossfading=%d\n",
                crossfade_state_.is_active() ? 1 : 0);

    // Get old slot before swap
    const ProgramSlot* old_slot = swap_controller_.current_slot();
    std::fprintf(stderr, "[VM] old_slot instruction_count=%u\n",
                old_slot ? old_slot->instruction_count : 0);

    // Execute the swap
    if (!swap_controller_.execute_swap()) {
        std::fprintf(stderr, "[VM] execute_swap returned false!\n");
        return;  // Swap failed
    }

    const ProgramSlot* new_slot = swap_controller_.current_slot();
    std::fprintf(stderr, "[VM] swap executed: new_slot instruction_count=%u, swap_count=%u\n",
                new_slot ? new_slot->instruction_count : 0,
                swap_controller_.swap_count());

    // Rebind states from old to new program
    rebind_states(old_slot, new_slot);

    // PRD prd-midi-input §7.3 GC-sweep fallback: any MidiQueueState that the
    // new program does NOT touch is about to be moved to the fading pool
    // when its host opcode disappears. Patch held-note durations to release
    // at the current block boundary so downstream consumers preserved by
    // the swap (e.g. a POLY that re-bound to a different midi source) see
    // finite note-off transitions instead of stuck sentinels.
    {
        const float spb_local = ctx_.samples_per_beat();
        const double spb_d = static_cast<double>(spb_local);
        const float now_beats = (spb_d > 0.0)
            ? static_cast<float>(
                  static_cast<double>(ctx_.global_sample_counter) / spb_d)
            : 0.0f;
        state_pool_.release_held_notes_on_untouched_midi(now_beats, spb_local);
    }

    // Determine if crossfade is needed
    if (old_slot && old_slot->instruction_count > 0 &&
        requires_crossfade(old_slot, new_slot)) {
        std::fprintf(stderr, "[VM] Starting crossfade, duration=%u blocks\n",
                    crossfade_config_.duration_blocks);
        crossfade_state_.begin(crossfade_config_.duration_blocks);
    } else {
        // No crossfade needed - immediately release previous slot
        // This prevents slot starvation when doing rapid non-structural changes
        std::fprintf(stderr, "[VM] No crossfade needed, releasing previous slot immediately\n");
        swap_controller_.release_previous();
    }
}

void VM::perform_crossfade(float* out_left, float* out_right) {
    // Zero crossfade buffers before executing programs into them.
    // OUTPUT opcode uses += accumulation; without this, audio from
    // previous blocks compounds across the crossfade duration.
    crossfade_buffers_.clear();

    // Get both program slots
    const ProgramSlot* old_slot = swap_controller_.previous_slot();
    const ProgramSlot* new_slot = swap_controller_.current_slot();

    // Deep-snapshot the entire live audio state so old + new programs
    // each advance from the same starting point. Two parts:
    //
    //   1. shadow_state_pool_ ← state_pool_  — the StateEntry table
    //      (OscState phase, EnvState stage, filter memory, delay
    //      write_pos, etc., plus bump-allocator side effects).
    //
    //   2. shadow_audio_arena_ ← audio_arena_  — every DSP buffer
    //      content (delay lines, reverb tank, sample voices, etc.)
    //      that DSP states point into. Arena base address never moves,
    //      so the pointers stored in state structs stay valid; only
    //      the bytes those pointers reference are restored.
    //
    // Without this, dual-execution against shared state + shared buffers
    // would double-mutate everything — clicks at swap boundary from
    // oscillator phase jumps, reverb tank corruption, etc.
    shadow_state_pool_.copy_states_from(state_pool_);
    [[maybe_unused]] const bool arena_snapshot_ok =
        shadow_audio_arena_.copy_used_from(audio_arena_);

    // Execute old program into crossfade buffers. Its state + arena
    // mutations happen in the live state_pool_ / audio_arena_; we
    // restore both from their shadows afterwards.
    if (old_slot && old_slot->instruction_count > 0) {
        execute_program(old_slot,
                       crossfade_buffers_.old_left.data(),
                       crossfade_buffers_.old_right.data());
    } else {
        std::fill(crossfade_buffers_.old_left.begin(),
                  crossfade_buffers_.old_left.end(), 0.0f);
        std::fill(crossfade_buffers_.old_right.begin(),
                  crossfade_buffers_.old_right.end(), 0.0f);
    }

    // Roll BOTH pool and arena back to the snapshot so the new program
    // sees the same starting state + buffer contents the old program saw.
    state_pool_.copy_states_from(shadow_state_pool_);
    if (arena_snapshot_ok) {
        (void) audio_arena_.copy_used_from(shadow_audio_arena_);
    }

    // Execute new program into crossfade buffers. Its state mutations
    // are the ones that stick.
    if (new_slot && new_slot->instruction_count > 0) {
        execute_program(new_slot,
                       crossfade_buffers_.new_left.data(),
                       crossfade_buffers_.new_right.data());
    } else {
        std::fill(crossfade_buffers_.new_left.begin(),
                  crossfade_buffers_.new_left.end(), 0.0f);
        std::fill(crossfade_buffers_.new_right.begin(),
                  crossfade_buffers_.new_right.end(), 0.0f);
    }

    // Mix with equal-power crossfade
    float position = crossfade_state_.position();
    crossfade_buffers_.mix_equal_power(out_left, out_right, position);
}

bool VM::requires_crossfade(const ProgramSlot* old_slot,
                           const ProgramSlot* new_slot) const {
    if (!old_slot || old_slot->instruction_count == 0) {
        // First program load - no crossfade needed
        return false;
    }

    // Skip the crossfade when the new bytecode is byte-identical to the
    // old. With deep-copy state preservation, NEW would produce the exact
    // same audio as OLD; running the equal-power dual-execution + mix
    // then introduces a (cos+sin) gain swell up to √2 at the midpoint
    // that does NOT exist in either signal alone, audibly clicking at
    // each block boundary inside the crossfade. ProgramSignature now
    // includes content_hash covering every instruction byte, so this
    // check catches every identity recompile (the dominant live-coding
    // case: re-evaluate the same source after an unrelated edit).
    //
    // Different content always crossfades — the equal-power gain swell
    // is the correct mix law for two uncorrelated signals.
    if (new_slot && old_slot->signature == new_slot->signature) {
        return false;
    }

    return true;
}

void VM::rebind_states([[maybe_unused]] const ProgramSlot* old_slot,
                      const ProgramSlot* new_slot) {
    // Mark states that exist in new program as touched
    // (This preserves them across the swap)
    // Note: old_slot reserved for future fade-out state tracking
    if (new_slot) {
        auto new_ids = new_slot->get_state_ids();
        for (auto id : new_ids) {
            if (state_pool_.exists(id)) {
                state_pool_.touch(id);
            }
        }
    }

    // GC will clean up orphaned states after crossfade completes
    // (handled by gc_sweep() called from hot_swap_end())
}

// Single-step dispatch of one instruction at body[ip], returning next ip.
// Owns the meta-opcodes that need access to the surrounding instruction
// stream — BLOCK_CALL, the BLOCK_BIND run that precedes a >5-param BLOCK_CALL,
// SKIP_IF_*, LOOP_STATIC — and RET-as-natural-end-of-body. Every other
// opcode falls through to VM::execute(). Driven by both `execute_program`
// (main stream) and every subprogram-body runner (`run_event_map_closure`,
// `run_event_filter_closure`, `run_voice_pool`, `run_foreach_*`,
// `execute_block_call`), so future meta-opcodes land in one place and are
// automatically available inside every body context.
std::size_t VM::execute_step(const ProgramSlot* slot,
                             std::span<const Instruction> body,
                             std::size_t ip) {
    const Instruction& inst = body[ip];
    switch (inst.opcode) {
      case Opcode::SKIP_IF_ZERO:
      case Opcode::SKIP_IF_NONZERO: {
        // Forward control flow: sample the predicate once per block at
        // sample [0] and skip ahead. Handled here, not via execute().
        const float* predicate = ctx_.buffers->get(inst.inputs[0]);
        return skip_if_next_ip(inst, predicate[0], ip);
      }
      case Opcode::LOOP_STATIC: {
        // Static loop: re-run the next body_len instructions `count` times.
        // body_len=rate, count=out_buffer. State is shared across passes.
        // Inner dispatch goes back through execute_step so nested meta-ops
        // (BLOCK_CALL etc.) inside the LOOP_STATIC body work too.
        const std::size_t body_len = inst.rate;
        const std::uint16_t iterations = inst.out_buffer;
        for (std::uint16_t it = 0; it < iterations; ++it) {
            std::size_t inner = ip + 1;
            const std::size_t inner_end = ip + 1 + body_len;
            while (inner < inner_end) {
                inner = execute_step(slot, body, inner);
            }
        }
        return ip + body_len + 1;
      }
      case Opcode::BLOCK_CALL:
        // L2: dispatch a shared user-`fn` body from the subprogram table.
        // The body is not inline — advance by one. `slot` must be non-null
        // for BLOCK_CALL to resolve; nullptr means the body came from a
        // legacy non-slot codepath and codegen guarantees no BLOCK_CALL.
        if (slot != nullptr) {
            execute_block_call(slot, body, ip);
        }
        return ip + 1;
      case Opcode::BLOCK_BIND: {
        // L2 >5-param call: a contiguous run of BLOCK_BIND carries the
        // logical param slots >=5 for the BLOCK_CALL that terminates the
        // run. Scan the run, then dispatch the BLOCK_CALL with the binds.
        const std::size_t bind_start = ip;
        const std::size_t end = body.size();
        std::size_t scan = ip;
        while (scan < end && body[scan].opcode == Opcode::BLOCK_BIND) {
            ++scan;
        }
        assert(scan < end && body[scan].opcode == Opcode::BLOCK_CALL &&
               "BLOCK_BIND run not terminated by BLOCK_CALL");
        if (slot != nullptr && scan < end &&
            body[scan].opcode == Opcode::BLOCK_CALL) {
            execute_block_call(slot, body, scan,
                               body.subspan(bind_start, scan - bind_start));
        }
        return scan + 1;
      }
      case Opcode::RET:
        // RET terminates a subprogram body. Body runners bound themselves
        // by body.size(), so reaching the final instruction naturally is
        // the normal exit — a mid-body RET means codegen emitted stray
        // control flow. In release, advancing to body.size() ends the loop.
        assert(ip + 1 == body.size() && "stray RET inside body");
        return body.size();
      default:
        execute(inst);
        return ip + 1;
    }
}

void VM::execute_program(const ProgramSlot* slot, float* out_left, float* out_right) {
    // Set output buffer pointers
    ctx_.output_left = out_left;
    ctx_.output_right = out_right;

    // Mark beginning of frame for state GC tracking
    state_pool_.begin_frame();

    // Execute all instructions (index-based for POLY block jumping).
    // The program span covers [ main | subprogram bodies ]; the main dispatch
    // loop runs only the main region — block bodies are reached via
    // FOREACH_EVENT / BLOCK_CALL dispatch, never fallen into.
    auto program = slot->program();
    // With no subprogram table the whole span is main program (back-compat,
    // including ProgramSlots constructed without load()). With a table, the
    // main loop stops at the main/body boundary.
    const std::size_t main_end = (slot->block_count == 0)
                                     ? program.size()
                                     : static_cast<std::size_t>(slot->main_count);
    auto main_span = program.subspan(0, main_end);
    std::size_t ip = 0;
    while (ip < main_end) {
        const Instruction& inst = main_span[ip];
        if (inst.opcode == Opcode::POLY_BEGIN) {
            ip = execute_poly_block(slot, program, ip);
        } else if (inst.opcode == Opcode::FOREACH_EVENT) {
            // L3 dispatch-loop opcode: run the subprogram body from the
            // table. The body is not inline — advance by exactly one.
            execute_foreach_event(slot, program, ip);
            ++ip;
        } else if (inst.opcode == Opcode::EVENT_MAP) {
            // prd-runtime-event-transforms: closure form needs slot->blocks[],
            // so it dispatches here; the Phase-1 packed form has no body.
            if (inst.flags & InstructionFlag::EVENT_CLOSURE) {
                run_event_map_closure(slot, inst);
            } else {
                op_event_map(ctx_, inst);
            }
            ++ip;
        } else if (inst.opcode == Opcode::EVENT_FILTER) {
            if (inst.flags & InstructionFlag::EVENT_CLOSURE) {
                run_event_filter_closure(slot, inst);
            } else {
                op_event_filter(ctx_, inst);
            }
            ++ip;
        } else {
            ip = execute_step(slot, main_span, ip);
        }
    }
}

std::size_t VM::execute_poly_block(const ProgramSlot* slot,
                                   std::span<const Instruction> program,
                                   std::size_t ip) {
    const auto& poly_inst = program[ip];
    const std::uint8_t body_length = poly_inst.rate;

    // Legacy POLY_BEGIN: body is inline at program[ip+1 .. ip+body_length].
    auto& poly_state = state_pool_.get_or_create<PolyAllocState>(poly_inst.state_id);
    run_voice_pool(slot,
                   poly_state,
                   poly_inst.out_buffer,
                   poly_inst.inputs[0], poly_inst.inputs[4],
                   program.subspan(ip + 1, body_length));

    // Advance past POLY_BEGIN + body + POLY_END
    return ip + 1 + body_length + 1;
}

// FOREACH_EVENT — table-based subprogram dispatch (PRD L3). The body lives in
// the ProgramSlot subprogram table, not inline; the dispatch loop advances by
// exactly one after this returns.
void VM::execute_foreach_event(const ProgramSlot* slot,
                               std::span<const Instruction> program,
                               std::size_t ip) {
    const auto& inst = program[ip];

    // Resolve the per-instance state. init_foreach_state created exactly one
    // of these three types for this state_id, so the first non-null wins and
    // selects the allocator kind.
    if (auto* poly_state = state_pool_.get_if<PolyAllocState>(inst.state_id)) {
        // VOICE_POOL — bit-exact with legacy POLY: identical run_voice_pool,
        // identical convention slots; only the body location differs.
        const auto body = slot->block_body(poly_state->block_id);
        run_voice_pool(slot,
                       *poly_state,
                       inst.out_buffer,
                       inst.inputs[0], inst.inputs[4],
                       body);
        return;
    }
    if (auto* iter_state = state_pool_.get_if<ForeachIterState>(inst.state_id)) {
        const auto body = slot->block_body(iter_state->block_id);
        run_foreach_per_iteration(slot, *iter_state, inst, body);
        return;
    }
    if (auto* shared_state = state_pool_.get_if<ForeachSharedState>(inst.state_id)) {
        const auto body = slot->block_body(shared_state->block_id);
        run_foreach_shared(slot, *shared_state, inst, body);
        return;
    }
    // No state — uninitialized FOREACH_EVENT (init_foreach_state did not run).
    // Skip silently; the dispatch loop still advances by one.
}

// BLOCK_CALL — table-based shared-`fn` dispatch (PRD L2). The body lives in
// the ProgramSlot subprogram table; the dispatch loop advances by one after
// this returns. Modeled on one iteration of run_foreach_per_iteration:
//   inst.rate       = block_id (subprogram table index)
//   inst.inputs[k]  = caller's argument buffer for parameter k
//   inst.out_buffer = caller's result buffer
//   inst.state_id   = per-call-site disambiguator (drives state isolation)
void VM::execute_block_call(const ProgramSlot* slot,
                            std::span<const Instruction> program,
                            std::size_t ip,
                            std::span<const Instruction> binds) {
    const auto& inst = program[ip];
    const std::uint32_t block_id = inst.rate;
    const auto body = slot->block_body(block_id);
    if (body.empty()) {
        // Out-of-range / corrupt block — skip silently (dispatch advances by 1).
        return;
    }
    const BlockEntry& entry = slot->blocks[block_id];

    // Copy each caller argument buffer into the body's fixed convention param
    // bank, so the once-compiled shared body reads its parameters from stable
    // indices regardless of which call site invoked it. The first 5 logical
    // slots travel in inputs[0..4]; slots >=5 (for >5-param fns) travel in the
    // preceding BLOCK_BIND run.
    const std::uint16_t slots = std::min<std::uint16_t>(entry.frame_slot_count, 5);
    for (std::uint16_t k = 0; k < slots; ++k) {
        const std::uint16_t src = inst.inputs[k];
        if (src == BUFFER_UNUSED) continue;
        const float* sb = buffer_pool_.get(src);
        float* db = buffer_pool_.get(entry.param_base + k);
        std::copy_n(sb, BLOCK_SIZE, db);
    }
    for (const auto& bind : binds) {
        const std::uint16_t slot_idx = bind.rate;
        if (slot_idx >= entry.frame_slot_count) continue;  // E248 guard
        const std::uint16_t src = bind.inputs[0];
        if (src == BUFFER_UNUSED) continue;
        const float* sb = buffer_pool_.get(src);
        float* db = buffer_pool_.get(entry.param_base + slot_idx);
        std::copy_n(sb, BLOCK_SIZE, db);
    }

    // Per-call-site state isolation. Compose with the entering XOR mask (rather
    // than assuming 0) so a BLOCK_CALL nested inside another isolated body —
    // a poly voice, a FOREACH iteration — still gets a distinct, restorable
    // projection. At top level the entering mask is 0, so this is bit-exact
    // with a plain set/reset.
    const std::uint32_t prev_xor = state_pool_.state_id_xor();
    state_pool_.set_state_id_xor(prev_xor ^ (inst.state_id * 0x9E3779B9u + 1));
    // Drive the body through execute_step so a nested BLOCK_CALL / BLOCK_BIND
    // run / SKIP_IF / LOOP_STATIC dispatches correctly (mutual recursion,
    // chained shared fns).
    std::size_t bi = 0;
    while (bi < body.size()) {
        bi = execute_step(slot, body, bi);
    }
    state_pool_.set_state_id_xor(prev_xor);

    // Copy the body output into the caller's result buffer.
    if (inst.out_buffer != BUFFER_UNUSED) {
        const float* ob = buffer_pool_.get(entry.body_output);
        std::copy_n(ob, BLOCK_SIZE, buffer_pool_.get(inst.out_buffer));
        if (entry.output_count == 2) {
            const float* ob_r = buffer_pool_.get(entry.body_output + 1);
            std::copy_n(ob_r, BLOCK_SIZE, buffer_pool_.get(inst.out_buffer + 1));
        }
    }
}

// Compute per-block cycle-position timing from the upstream cycle length.
// Extracted from run_voice_pool so the FOREACH_EVENT PER_ITERATION runner can
// reuse the exact same onset/offset math for synthesized gate/trig.
VM::BlockCycleTiming VM::compute_block_timing(float cycle_length) const {
    // Cycle-position snap tolerance, in beats. IEEE-754 division of large
    // sample counters by spb produces tiny non-zero fmod residues at exact
    // cycle boundaries; those residues are far below 1-sample resolution but
    // break strict comparisons against zero-time events. Snap within an
    // epsilon well below sample resolution but well above fp noise.
    constexpr double CYCLE_BOUNDARY_EPSILON = 1e-9;
    BlockCycleTiming t{};
#ifdef CEDAR_FLOAT_ONLY
    // Float-only beat timing (precision degrades after ~6 min at 48kHz)
    const float spb = (60.0f / ctx_.bpm) * ctx_.sample_rate;
    const float beat_start =
        static_cast<float>(ctx_.global_sample_counter) / spb;
    float cycle_pos_raw = std::fmod(beat_start, cycle_length);
    if (cycle_pos_raw < static_cast<float>(CYCLE_BOUNDARY_EPSILON)) {
        cycle_pos_raw = 0.0f;
    }
    t.cycle_pos = cycle_pos_raw;
    t.current_cycle =
        static_cast<std::uint32_t>(std::floor(beat_start / cycle_length));
    float block_end_pos_raw = t.cycle_pos + static_cast<float>(BLOCK_SIZE) / spb;
    if (std::abs(block_end_pos_raw - cycle_length)
            < static_cast<float>(CYCLE_BOUNDARY_EPSILON)) {
        block_end_pos_raw = cycle_length;
    }
    t.block_end_pos = block_end_pos_raw;
    t.spb = spb;
#else
    // Use double precision for beat timing to avoid float32 precision loss
    // after ~6 minutes (global_sample_counter > 2^24)
    const double spb_d = (60.0 / static_cast<double>(ctx_.bpm))
                       * static_cast<double>(ctx_.sample_rate);
    const double beat_start_d =
        static_cast<double>(ctx_.global_sample_counter) / spb_d;
    double cycle_pos_d_raw =
        std::fmod(beat_start_d, static_cast<double>(cycle_length));
    if (cycle_pos_d_raw < CYCLE_BOUNDARY_EPSILON) cycle_pos_d_raw = 0.0;
    const double cycle_pos_d = cycle_pos_d_raw;
    t.current_cycle =
        static_cast<std::uint32_t>(std::floor(beat_start_d / cycle_length));
    const double block_beats_d = static_cast<double>(BLOCK_SIZE) / spb_d;
    double block_end_pos_d_raw = cycle_pos_d + block_beats_d;
    if (std::abs(block_end_pos_d_raw - static_cast<double>(cycle_length))
            < CYCLE_BOUNDARY_EPSILON) {
        block_end_pos_d_raw = static_cast<double>(cycle_length);
    }
    // Narrow to float for comparisons against float-precision event times
    t.spb = static_cast<float>(spb_d);
    t.cycle_pos = static_cast<float>(cycle_pos_d);
    t.block_end_pos = static_cast<float>(block_end_pos_d_raw);
#endif
    return t;
}

void VM::run_voice_pool(const ProgramSlot* slot,
                        PolyAllocState& poly_state,
                        std::uint16_t mix_buf,
                        std::uint16_t bank_base,
                        std::uint16_t voice_out_buf,
                        std::span<const Instruction> body) {
    // poly is stereo-native: voice_out and mix are adjacent L/R pairs. The
    // opcode has no free input slots, so the R buffers are derived via the +1
    // adjacency convention (codegen guarantees the pairs are adjacent).
    std::uint16_t mix_buf_r = mix_buf + 1;
    std::uint16_t voice_out_buf_r = voice_out_buf + 1;

    // The per-voice field bank is contiguous from bank_base, indexed by
    // PatternPayload field id: 0 FREQ, 1 VEL, 2 TRIG, 3 GATE, 4 TYPE,
    // 5 NOTE, 6 DUR, 7 CHANCE, 8 TIME, 9 PHASE, 10 SAMPLE_ID, followed by
    // poly_state.prop_count custom record-suffix property slots (Phase 3).

    poly_state.ensure_voices(ctx_.arena);

    // Clear both mix channels to zero
    float* mix = buffer_pool_.get(mix_buf);
    float* mix_r = buffer_pool_.get(mix_buf_r);
    std::fill_n(mix, BLOCK_SIZE, 0.0f);
    std::fill_n(mix_r, BLOCK_SIZE, 0.0f);

    // =========================================================================
    // Event processing: read OutputEvents from linked event source
    //
    // Resolves to either a SequenceState (pattern upstream) or a
    // MidiQueueState (MIDI_QUERY upstream) — both expose the same
    // `OutputEvents` shape, so the voice-allocation logic below is
    // source-agnostic per the PolyAllocState contract in
    // prd-polyphony-system §2.6.
    // =========================================================================
    auto events_src = (poly_state.seq_state_id != 0)
        ? state_pool_.resolve_output_events(poly_state.seq_state_id)
        : StatePool::ResolvedEvents{};

    if (events_src.events && events_src.events->num_events > 0) {
        OutputEvents& seq_output = *events_src.events;
        const float cycle_length = events_src.cycle_length;
        const BlockCycleTiming timing = compute_block_timing(cycle_length);
        const float spb = timing.spb;
        const float cycle_pos = timing.cycle_pos;
        const float block_end_pos = timing.block_end_pos;
        const std::uint32_t current_cycle = timing.current_cycle;

        // Reset pending gate transitions for all voices
        if (poly_state.voices) {
            for (std::uint16_t v = 0; v < poly_state.max_voices; ++v) {
                poly_state.voices[v].pending_gate_on = BLOCK_SIZE;
                poly_state.voices[v].pending_gate_off = BLOCK_SIZE;
            }
        }

        // Scan all output events for gate-on and gate-off within this block
        for (std::uint32_t e = 0; e < seq_output.num_events; ++e) {
            const auto& evt = seq_output.events[e];

            float evt_start = evt.time;
            float evt_end = evt.time + evt.duration;

            // Check for gate-on: event starts within [cycle_pos, block_end_pos)
            bool gate_on_this_block = false;
            float on_beat_offset = 0.0f;
            std::uint32_t on_cycle = current_cycle;

            if (evt_start >= cycle_pos && evt_start < block_end_pos) {
                // Normal case: event starts within block
                gate_on_this_block = true;
                on_beat_offset = evt_start - cycle_pos;
            } else if (block_end_pos > cycle_length) {
                // Block wraps around cycle boundary — event is in next cycle
                float wrapped_end = block_end_pos - cycle_length;
                if (evt_start < wrapped_end) {
                    gate_on_this_block = true;
                    on_beat_offset = (cycle_length - cycle_pos) + evt_start;
                    on_cycle = current_cycle + 1;
                }
            }

            if (gate_on_this_block) {
                std::uint32_t on_sample = static_cast<std::uint32_t>(
                    std::max(0.0f, on_beat_offset * spb));
                if (on_sample >= BLOCK_SIZE) on_sample = BLOCK_SIZE - 1;

                // Allocate a voice for each chord note in this event. The
                // per-voice .note is the chord voice's own MIDI note for a
                // multi-value event, else the event-wide midi_note.
                for (std::uint8_t vi = 0; vi < evt.num_values; ++vi) {
                    const float note_in = (evt.num_values > 1)
                        ? evt.notes[vi] : evt.midi_note;
                    poly_state.allocate_voice(
                        evt.values[vi], evt.velocity,
                        static_cast<std::uint16_t>(e), on_cycle, on_sample,
                        evt.duration, evt.chance, evt.time,
                        static_cast<float>(evt.type_id), note_in,
                        ctx_.global_sample_counter + on_sample,
                        evt.prop_vals, evt.prop_set_mask);
                }
            }

            // Check for gate-off: event ends within [cycle_pos, block_end_pos)
            bool gate_off_this_block = false;
            float off_beat_offset = 0.0f;
            std::uint32_t off_cycle = current_cycle;

            if (evt_end >= cycle_pos && evt_end < block_end_pos) {
                gate_off_this_block = true;
                off_beat_offset = evt_end - cycle_pos;
            } else if (evt_end >= cycle_length && current_cycle > 0) {
                // Event ends at or past the cycle boundary, releasing a voice
                // allocated in the previous cycle. The `>=` covers the
                // exact-alignment edge case (BPMs where cycle_length_in_samples
                // is a multiple of BLOCK_SIZE): at those tempos the cycle
                // boundary lands exactly on a block boundary, and we want the
                // gate-off to fire in the new cycle's first block (sample 0)
                // alongside the new cycle's gate-ons, not in the previous
                // block's last sample. The `current_cycle > 0` guard prevents
                // releasing a just-allocated voice in the very first cycle —
                // there is no previous cycle whose voice needs releasing.
                float wrapped_end = evt_end - cycle_length;
                if (wrapped_end >= cycle_pos && wrapped_end < block_end_pos) {
                    gate_off_this_block = true;
                    off_beat_offset = wrapped_end - cycle_pos;
                    off_cycle = current_cycle - 1;
                } else if (block_end_pos > cycle_length) {
                    float block_wrapped = block_end_pos - cycle_length;
                    if (wrapped_end < block_wrapped) {
                        gate_off_this_block = true;
                        off_beat_offset = (cycle_length - cycle_pos) + wrapped_end;
                    }
                }
            }

            if (gate_off_this_block) {
                std::uint32_t off_sample = static_cast<std::uint32_t>(
                    std::max(0.0f, off_beat_offset * spb));
                if (off_sample >= BLOCK_SIZE) off_sample = BLOCK_SIZE - 1;

                poly_state.release_voice_by_event(
                    static_cast<std::uint16_t>(e), off_cycle, off_sample);
            }
        }

        // Age voices and clean up timed-out releases
        poly_state.tick();
    }

    // =========================================================================
    // Voice iteration: fill buffers and execute body per active voice
    // =========================================================================
    for (std::uint8_t v = 0; v < poly_state.max_voices; ++v) {
        if (!poly_state.voices || !poly_state.voices[v].active) continue;

        auto& voice = poly_state.voices[v];

        // Fill the per-voice field bank (indexed by PatternPayload field id).
        // The 11 fixed fields first: FREQ/VEL/TYPE/NOTE/DUR/CHANCE/TIME/PHASE/
        // SAMPLE_ID are event-scalar (block-constant); GATE/TRIG are
        // synthesized per-sample. Custom prop slots follow (filled below).
        float* freq_buf = buffer_pool_.get(bank_base + 0);
        float* vel_buf  = buffer_pool_.get(bank_base + 1);
        float* trig_buf = buffer_pool_.get(bank_base + 2);
        float* gate_buf = buffer_pool_.get(bank_base + 3);

        std::fill_n(freq_buf, BLOCK_SIZE, voice.freq);
        std::fill_n(vel_buf, BLOCK_SIZE, voice.vel);
        std::fill_n(trig_buf, BLOCK_SIZE, 0.0f);
        std::fill_n(buffer_pool_.get(bank_base + 4), BLOCK_SIZE, voice.type_id);
        std::fill_n(buffer_pool_.get(bank_base + 5), BLOCK_SIZE, voice.note_value);
        std::fill_n(buffer_pool_.get(bank_base + 6), BLOCK_SIZE, voice.duration);
        std::fill_n(buffer_pool_.get(bank_base + 7), BLOCK_SIZE, voice.chance);
        std::fill_n(buffer_pool_.get(bank_base + 8), BLOCK_SIZE, voice.time);
        // SAMPLE_ID shares the primary value slot with FREQ (for sample
        // patterns evt.values[vi] is the sample id).
        std::fill_n(buffer_pool_.get(bank_base + 10), BLOCK_SIZE, voice.freq);

        // PHASE — block-constant voice progress (0..1) through its event
        // duration, derived from the absolute onset sample.
        {
            const float spb = (60.0f / ctx_.bpm) * ctx_.sample_rate;
            const float dur_samp = voice.duration * spb;
            double elapsed = static_cast<double>(ctx_.global_sample_counter)
                           - static_cast<double>(voice.onset_sample);
            if (elapsed < 0.0) elapsed = 0.0;
            const float phase = dur_samp > 0.0f
                ? std::clamp(static_cast<float>(elapsed / dur_samp),
                             0.0f, 1.0f)
                : 0.0f;
            std::fill_n(buffer_pool_.get(bank_base + 9), BLOCK_SIZE, phase);
        }

        // Custom record-suffix property slots (Phase 3) — contiguous after
        // the 11 fixed fields. Each voice reads its own event's prop value
        // when the presence bit is set, else the callback destructure
        // default. Block-constant per voice.
        for (std::uint8_t s = 0; s < poly_state.prop_count; ++s) {
            const float prop_v = (voice.prop_set_mask & (1u << s))
                ? voice.prop_vals[s]
                : poly_state.prop_defaults[s];
            const std::uint16_t prop_buf = static_cast<std::uint16_t>(
                bank_base + 11 + s);
            std::fill_n(buffer_pool_.get(prop_buf), BLOCK_SIZE, prop_v);
        }

        // Per-sample gate and trigger accuracy
        if (voice.pending_gate_on < BLOCK_SIZE) {
            // Note-on happened this block
            std::fill_n(gate_buf, voice.pending_gate_on, 0.0f);
            std::fill_n(gate_buf + voice.pending_gate_on,
                        BLOCK_SIZE - voice.pending_gate_on, 1.0f);
            trig_buf[voice.pending_gate_on] = 1.0f;
            // When the note-on lands at sample 0 (chord transition coincides
            // with a block boundary, e.g. at BPMs where cycle_length_in_samples
            // is a multiple of BLOCK_SIZE), the gate buffer would be all 1s
            // with no 0->1 edge. For voices retriggered from the previous
            // chord (shared frequencies), the previous block ended with gate=1
            // too, so envelope opcodes (e.g. ENV_AR) never see a rising edge
            // and fail to retrigger. Force a one-sample drop at the boundary
            // so the rising edge appears at sample 1; the new envelope attack
            // is delayed by one sample (~21µs) but actually fires.
            if (voice.pending_gate_on == 0) {
                gate_buf[0] = 0.0f;
            }
        } else if (voice.pending_gate_off < BLOCK_SIZE) {
            // Note-off happened this block
            std::fill_n(gate_buf, voice.pending_gate_off, 1.0f);
            std::fill_n(gate_buf + voice.pending_gate_off,
                        BLOCK_SIZE - voice.pending_gate_off, 0.0f);
        } else {
            // No transition: fill with current gate state
            std::fill_n(gate_buf, BLOCK_SIZE, voice.gate);
        }

        // Set XOR isolation for this voice
        state_pool_.set_state_id_xor(
            static_cast<std::uint32_t>(v) * 0x9E3779B9u + 1);

        // Execute body instructions through execute_step so BLOCK_CALL,
        // BLOCK_BIND runs, SKIP_IF, LOOP_STATIC inside a voice body dispatch.
        std::size_t bi = 0;
        while (bi < body.size()) {
            bi = execute_step(slot, body, bi);
        }

        // Accumulate voice output into the stereo mix, gated by the (mono)
        // gate signal applied identically to both channels.
        //
        // Release-window override (PRD prd-midi-input §7.2): while a voice's
        // release_countdown is positive, the mix-side gate is held at 1.0
        // even though the per-sample gate_buf already stepped 1→0 at
        // note-off. The voice body's ADSR sees the 1→0 edge and runs its
        // release tail naturally; the mix keeps summing it. When the
        // countdown expires, the actual gate value (0) zeros the output
        // and tick() reaps the slot.
        const float* voice_out = buffer_pool_.get(voice_out_buf);
        const float* voice_out_r = buffer_pool_.get(voice_out_buf_r);
        const float* gate = gate_buf;  // bank_base + 3 (GATE)
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            float g;
            if (voice.release_countdown > 0) {
                g = 1.0f;
                --voice.release_countdown;
            } else {
                g = gate[i];
            }
            mix[i]   += voice_out[i]   * g;
            mix_r[i] += voice_out_r[i] * g;
        }
    }

    // Reset XOR
    state_pool_.set_state_id_xor(0);
}

// Fill the 7-buffer event-record bank for one FOREACH_EVENT iteration.
// Layout (contiguous from bank_buf): vel, dur, note, chance, time, gate, trig.
void VM::fill_event_record_bank(std::uint16_t bank_buf,
                                const OutputEvents::OutputEvent& evt,
                                const BlockCycleTiming& timing) {
    std::fill_n(buffer_pool_.get(bank_buf + EVENT_BANK_VEL),    BLOCK_SIZE, evt.velocity);
    std::fill_n(buffer_pool_.get(bank_buf + EVENT_BANK_DUR),    BLOCK_SIZE, evt.duration);
    std::fill_n(buffer_pool_.get(bank_buf + EVENT_BANK_NOTE),   BLOCK_SIZE, evt.midi_note);
    std::fill_n(buffer_pool_.get(bank_buf + EVENT_BANK_CHANCE), BLOCK_SIZE, evt.chance);
    std::fill_n(buffer_pool_.get(bank_buf + EVENT_BANK_TIME),   BLOCK_SIZE, evt.time);

    // Synthesize gate (held high across the event window) and trig (1-sample
    // pulse at onset) from the event's cycle-relative time/duration. The
    // event time is cycle-relative (range [0, cycle_length)); rel_* are sample
    // offsets from this block's start. Cross-cycle-boundary wrap is not
    // modelled here — the gate edge appears in the block the onset lands in.
    float* gate_b = buffer_pool_.get(bank_buf + EVENT_BANK_GATE);
    float* trig_b = buffer_pool_.get(bank_buf + EVENT_BANK_TRIG);
    const float rel_start = (evt.time - timing.cycle_pos) * timing.spb;
    const float rel_end =
        (evt.time + evt.duration - timing.cycle_pos) * timing.spb;
    const int on_sample  = static_cast<int>(std::floor(rel_start));
    const int off_sample = static_cast<int>(std::floor(rel_end));
    const int block = static_cast<int>(BLOCK_SIZE);
    const int on_clamped  = std::clamp(on_sample, 0, block);
    const int off_clamped = std::clamp(off_sample, 0, block);
    for (int i = 0; i < block; ++i) {
        gate_b[i] = (i >= on_clamped && i < off_clamped) ? 1.0f : 0.0f;
    }
    std::fill_n(trig_b, BLOCK_SIZE, 0.0f);
    if (on_sample >= 0 && on_sample < block) {
        trig_b[on_sample] = 1.0f;
    }

    // Phase 5 Commit D: chord-array slots. The closure body reads `e.notes`
    // (DynArray onto evt.notes[]) and `e.freqs` (DynArray onto evt.values[])
    // as data buffers packed in samples 0..num_values-1, with num_values
    // broadcast on the shared length buffer.
    float* notes_data = buffer_pool_.get(bank_buf + EVENT_BANK_NOTES_DATA);
    float* freqs_data = buffer_pool_.get(bank_buf + EVENT_BANK_FREQS_DATA);
    float* num_vals   = buffer_pool_.get(bank_buf + EVENT_BANK_NUM_VALUES);
    const std::uint8_t nv = evt.num_values;
    const std::uint8_t copy_n = std::min<std::uint8_t>(nv, MAX_VALUES_PER_EVENT);
    for (std::uint8_t k = 0; k < copy_n; ++k) {
        notes_data[k] = evt.notes[k];
        freqs_data[k] = evt.values[k];
    }
    // Zero the unused tail of the packed data buffers so an off-by-one
    // ARRAY_INDEX read returns 0 rather than stale data from a prior event.
    for (std::size_t k = copy_n; k < BLOCK_SIZE; ++k) {
        notes_data[k] = 0.0f;
        freqs_data[k] = 0.0f;
    }
    std::fill_n(num_vals, BLOCK_SIZE, static_cast<float>(nv));
}

// PER_ITERATION — every upstream event maps to one iteration in this block.
// Per-iteration DSP state is isolated via state_id XOR, exactly as
// run_voice_pool isolates voices. Slot layout: inputs[0]=freq (primary),
// inputs[1]=record bank base (or UNUSED), inputs[4]=voice_out L. When
// out_buffer is UNUSED the body is a side-effecting sink (`each()`): the
// body's own out() calls accumulate into the global bus, no mixing here.
void VM::run_foreach_per_iteration(const ProgramSlot* slot,
                                   ForeachIterState& iter_state,
                                   const Instruction& inst,
                                   std::span<const Instruction> body) {
    const std::uint16_t freq_buf = inst.inputs[0];
    const std::uint16_t bank_buf = inst.inputs[1];
    const std::uint16_t out_buf  = inst.out_buffer;
    const bool mixed = (out_buf != BUFFER_UNUSED);

    float* mix   = nullptr;
    float* mix_r = nullptr;
    if (mixed) {
        mix   = buffer_pool_.get(out_buf);
        mix_r = buffer_pool_.get(out_buf + 1);
        std::fill_n(mix, BLOCK_SIZE, 0.0f);
        std::fill_n(mix_r, BLOCK_SIZE, 0.0f);
    }

    auto events_src = (iter_state.seq_state_id != 0)
        ? state_pool_.resolve_output_events(iter_state.seq_state_id)
        : StatePool::ResolvedEvents{};
    if (!events_src.events || events_src.events->num_events == 0) {
        return;
    }

    OutputEvents& seq_output = *events_src.events;
    const std::uint32_t iter_cap =
        std::min<std::uint32_t>(seq_output.num_events, iter_state.max_iterations);
    const BlockCycleTiming timing =
        compute_block_timing(events_src.cycle_length);

    // Voice-out buffer the body writes its signal into (slot 4, like POLY).
    const std::uint16_t voice_out_buf = inst.inputs[4];
    const std::uint16_t voice_out_buf_r = voice_out_buf + 1;

    for (std::uint32_t e = 0; e < iter_cap; ++e) {
        const auto& evt = seq_output.events[e];

        // Bind the primary (freq) convention slot + the optional record bank.
        if (freq_buf != BUFFER_UNUSED) {
            std::fill_n(buffer_pool_.get(freq_buf), BLOCK_SIZE,
                        evt.num_values > 0 ? evt.values[0] : evt.midi_note);
        }
        if (bank_buf != BUFFER_UNUSED) {
            fill_event_record_bank(bank_buf, evt, timing);
        }

        // Per-iteration state isolation — same XOR scheme as run_voice_pool.
        state_pool_.set_state_id_xor(e * 0x9E3779B9u + 1);
        std::size_t bi = 0;
        while (bi < body.size()) {
            bi = execute_step(slot, body, bi);
        }

        if (mixed && voice_out_buf != BUFFER_UNUSED) {
            const float* vo   = buffer_pool_.get(voice_out_buf);
            const float* vo_r = buffer_pool_.get(voice_out_buf_r);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                mix[i]   += vo[i];
                mix_r[i] += vo_r[i];
            }
        }
    }

    state_pool_.set_state_id_xor(0);
}

// SHARED — all iterations share one accumulator slot (fold). No per-iteration
// XOR; the body runs once per event with the accumulator threaded through.
// Slot layout: inputs[0]=accumulator, inputs[1]=seed signal (or UNUSED),
// inputs[2]=freq (primary), inputs[3]=record bank base (or UNUSED). The
// accumulator is re-seeded from the seed signal every block, so fold is a
// per-block reduction starting from the seed's current value.
void VM::run_foreach_shared(const ProgramSlot* slot,
                            ForeachSharedState& shared_state,
                            const Instruction& inst,
                            std::span<const Instruction> body) {
    const std::uint16_t acc_buf  = inst.inputs[0];
    const std::uint16_t seed_buf = inst.inputs[1];
    const std::uint16_t freq_buf = inst.inputs[2];
    const std::uint16_t bank_buf = inst.inputs[3];
    const std::uint16_t out_buf  = inst.out_buffer;

    // Re-seed the accumulator each block from the seed signal's current value.
    float acc = (seed_buf != BUFFER_UNUSED)
        ? buffer_pool_.get(seed_buf)[0]
        : shared_state.accumulator;

    auto events_src = (shared_state.seq_state_id != 0)
        ? state_pool_.resolve_output_events(shared_state.seq_state_id)
        : StatePool::ResolvedEvents{};

    if (events_src.events) {
        OutputEvents& seq_output = *events_src.events;
        const BlockCycleTiming timing =
            compute_block_timing(events_src.cycle_length);
        for (std::uint32_t e = 0; e < seq_output.num_events; ++e) {
            const auto& evt = seq_output.events[e];
            // Thread the accumulator into the body's accumulator slot, run
            // the body once, read the updated accumulator back out.
            if (acc_buf != BUFFER_UNUSED) {
                std::fill_n(buffer_pool_.get(acc_buf), BLOCK_SIZE, acc);
            }
            if (freq_buf != BUFFER_UNUSED) {
                std::fill_n(buffer_pool_.get(freq_buf), BLOCK_SIZE,
                            evt.num_values > 0 ? evt.values[0] : evt.midi_note);
            }
            if (bank_buf != BUFFER_UNUSED) {
                fill_event_record_bank(bank_buf, evt, timing);
            }
            std::size_t bi = 0;
            while (bi < body.size()) {
                bi = execute_step(slot, body, bi);
            }
            acc = buffer_pool_.get(out_buf)[0];
        }
    }

    shared_state.accumulator = acc;
    // Publish the running accumulator to the output buffer.
    std::fill_n(buffer_pool_.get(out_buf), BLOCK_SIZE, acc);
}

// Closure EVENT_MAP (PRD prd-runtime-event-transforms Phase 2). For each
// upstream event: copy it through, bind the closure's event-record input
// buffers, run the closure subprogram body once under per-event XOR isolation,
// then overlay the closure-assigned output fields. See event_transform_encoding.hpp
// for the instruction layout.
void VM::run_event_map_closure(const ProgramSlot* slot, const Instruction& inst) {
    auto& dst = state_pool_.get_or_create<SequenceState>(inst.state_id);
    auto src = state_pool_.resolve_output_events(event_transform_upstream_id(inst));
    if (!src.events) {
        dst.output.clear();
        dst.current_index = 0;
        return;
    }
    dst.cycle_length = src.cycle_length;

    const auto body = slot->block_body(inst.rate);
    const BlockCycleTiming timing = compute_block_timing(src.cycle_length);
    const std::uint16_t freq_buf = inst.inputs[0];
    const std::uint16_t bank_buf = inst.inputs[1];
    const std::uint16_t out_bank = inst.inputs[4];
    // Mask widened to 16 bits in Phase 5 Commit E: EVENT_OUT_COUNT == 10
    // means slots 0..9 occupy mask bits 0..9 (instruction-flag bits 4..13).
    const std::uint16_t mask =
        static_cast<std::uint16_t>((inst.flags >> InstructionFlag::EVENT_MASK_SHIFT)
                                   & ((1u << EVENT_OUT_COUNT) - 1u));

    // Chord-array overlay decoded from the mask once per block (constant
    // across all events; the closure's write shape doesn't depend on event).
    const bool write_notes  = (mask & (1u << EVENT_OUT_NOTES_DATA)) != 0;
    const bool write_freqs  = (mask & (1u << EVENT_OUT_FREQS_DATA)) != 0;
    const bool write_chord  = write_notes || write_freqs;

    dst.output.clear();
    const std::uint32_t n = std::min(src.events->num_events, dst.output.capacity);
    const std::uint32_t prev_xor = state_pool_.state_id_xor();
    for (std::uint32_t i = 0; i < n; ++i) {
        OutputEvents::OutputEvent e = src.events->events[i];  // copy-through

        if (freq_buf != BUFFER_UNUSED) {
            std::fill_n(buffer_pool_.get(freq_buf), BLOCK_SIZE,
                        e.num_values > 0 ? e.values[0] : e.midi_note);
        }
        if (bank_buf != BUFFER_UNUSED) {
            fill_event_record_bank(bank_buf, e, timing);
        }

        state_pool_.set_state_id_xor(prev_xor ^ (i * 0x9E3779B9u + 1));
        std::size_t bi = 0;
        while (bi < body.size()) {
            bi = execute_step(slot, body, bi);
        }

        if (out_bank != BUFFER_UNUSED) {
            // Scalar slots first (NOTE..AT). Codegen drops the NOTE COPY when
            // a chord array is also written (notes/freqs wins precedence —
            // PRD prd-runtime-event-transforms.md Phase 5 Commit E), so this
            // loop is naturally safe to run before the chord overlay.
            for (std::uint8_t s = 0; s < EVENT_OUT_NOTES_DATA; ++s) {
                if (!((mask >> s) & 1u)) continue;
                overlay_event_field(e, s, buffer_pool_.get(out_bank + s)[0]);
            }
            // Chord-array overlay. Read the shared length and clamp to the
            // per-event capacity; oversize arrays clamp silently (live-coding
            // coerces, doesn't fail).
            if (write_chord) {
                const float nv_f =
                    buffer_pool_.get(out_bank + EVENT_OUT_NUM_VALUES)[0];
                std::uint8_t nv = (nv_f >= 1.0f)
                    ? static_cast<std::uint8_t>(nv_f) : std::uint8_t{0};
                if (nv > MAX_VALUES_PER_EVENT) {
                    nv = static_cast<std::uint8_t>(MAX_VALUES_PER_EVENT);
                }
                const float* notes_buf = write_notes
                    ? buffer_pool_.get(out_bank + EVENT_OUT_NOTES_DATA)
                    : nullptr;
                const float* freqs_buf = write_freqs
                    ? buffer_pool_.get(out_bank + EVENT_OUT_FREQS_DATA)
                    : nullptr;
                if (write_notes && write_freqs) {
                    // Both written — copy each through verbatim. Closure
                    // author is responsible for keeping them consistent.
                    for (std::uint8_t k = 0; k < nv; ++k) {
                        e.notes[k]  = notes_buf[k];
                        e.values[k] = freqs_buf[k];
                    }
                } else if (write_notes) {
                    // Notes-only: derive Hz frequencies via mtof so downstream
                    // voicing reads stay consistent.
                    for (std::uint8_t k = 0; k < nv; ++k) {
                        const float midi = notes_buf[k];
                        e.notes[k]  = midi;
                        e.values[k] = 440.0f *
                            std::pow(2.0f, (midi - 69.0f) / 12.0f);
                    }
                } else {
                    // Freqs-only: derive MIDI from Hz via ftom (log2-based).
                    for (std::uint8_t k = 0; k < nv; ++k) {
                        const float hz = freqs_buf[k];
                        e.values[k] = hz;
                        e.notes[k]  = (hz > 0.0f)
                            ? 69.0f + 12.0f * std::log2(hz / 440.0f)
                            : 0.0f;
                    }
                }
                // Clear the stale tail when the new chord is shorter than
                // the upstream copy-through. Without this a shrinking
                // closure would leave old voices visible downstream.
                for (std::uint8_t k = nv; k < e.num_values; ++k) {
                    e.notes[k]  = 0.0f;
                    e.values[k] = 0.0f;
                }
                e.num_values = nv;
                if (nv > 0) {
                    // Keep the primary scalar in sync with notes[0] / values[0]
                    // so downstream reads of `e.note` / `e.freq` (scalar)
                    // reflect the chord's lead voice.
                    e.midi_note = e.notes[0];
                }
            }
        }
        dst.output.events[i] = e;
    }
    dst.output.num_events = n;
    state_pool_.set_state_id_xor(prev_xor);

    // Only a TIME rewrite can reorder events.
    if (mask & (1u << EVENT_OUT_TIME)) {
        dst.output.sort_by_time();
    }
    event_transform_reposition(dst, ctx_);
}

// Closure EVENT_FILTER (PRD prd-runtime-event-transforms Phase 2). The closure
// body writes a predicate value into inputs[4]; events copy through only when
// the predicate is non-zero.
void VM::run_event_filter_closure(const ProgramSlot* slot, const Instruction& inst) {
    auto& dst = state_pool_.get_or_create<SequenceState>(inst.state_id);
    auto src = state_pool_.resolve_output_events(event_transform_upstream_id(inst));
    if (!src.events) {
        dst.output.clear();
        dst.current_index = 0;
        return;
    }
    dst.cycle_length = src.cycle_length;

    const auto body = slot->block_body(inst.rate);
    const BlockCycleTiming timing = compute_block_timing(src.cycle_length);
    const std::uint16_t freq_buf = inst.inputs[0];
    const std::uint16_t bank_buf = inst.inputs[1];
    const std::uint16_t pred_buf = inst.inputs[4];

    dst.output.clear();
    std::uint32_t written = 0;
    const std::uint32_t prev_xor = state_pool_.state_id_xor();
    for (std::uint32_t i = 0;
         i < src.events->num_events && written < dst.output.capacity; ++i) {
        const OutputEvents::OutputEvent& e = src.events->events[i];

        if (freq_buf != BUFFER_UNUSED) {
            std::fill_n(buffer_pool_.get(freq_buf), BLOCK_SIZE,
                        e.num_values > 0 ? e.values[0] : e.midi_note);
        }
        if (bank_buf != BUFFER_UNUSED) {
            fill_event_record_bank(bank_buf, e, timing);
        }

        state_pool_.set_state_id_xor(prev_xor ^ (i * 0x9E3779B9u + 1));
        std::size_t bi = 0;
        while (bi < body.size()) {
            bi = execute_step(slot, body, bi);
        }

        const bool keep = (pred_buf != BUFFER_UNUSED) &&
                          (buffer_pool_.get(pred_buf)[0] != 0.0f);
        if (keep) {
            dst.output.events[written++] = e;
        }
    }
    dst.output.num_events = written;
    state_pool_.set_state_id_xor(prev_xor);
    event_transform_reposition(dst, ctx_);
}

void VM::execute(const Instruction& inst) {
    // Every audio-signal opcode is stereo-native (prd-stereo-native-opcodes
    // Phase 5): it handles both channels in one dispatch with one state struct,
    // reading the STEREO_INPUT / STEREO_OUTPUT flags itself. The legacy
    // auto-lift dispatch (run-the-opcode-twice with XOR'd state_id) is retired.

    // Switch dispatch - compiler generates jump table for O(1) dispatch
    // [[likely]] hints help branch prediction for common opcodes
    switch (inst.opcode) {
        // === Stack/Constants ===
        case Opcode::NOP:
            break;

        case Opcode::PUSH_CONST:
            op_push_const(ctx_, inst);
            break;

        case Opcode::COPY:
            op_copy(ctx_, inst);
            break;

        // === Arithmetic ===
        [[likely]] case Opcode::ADD:
            op_add(ctx_, inst);
            break;

        [[likely]] case Opcode::SUB:
            op_sub(ctx_, inst);
            break;

        [[likely]] case Opcode::MUL:
            op_mul(ctx_, inst);
            break;

        case Opcode::DIV:
            op_div(ctx_, inst);
            break;

        case Opcode::POW:
            op_pow(ctx_, inst);
            break;

        case Opcode::NEG:
            op_neg(ctx_, inst);
            break;

        // === Oscillators ===
        [[likely]] case Opcode::OSC_SIN:
            op_osc_sin(ctx_, inst);
            break;

        case Opcode::OSC_TRI:
            op_osc_tri(ctx_, inst);
            break;

        case Opcode::OSC_SAW:
            op_osc_saw(ctx_, inst);
            break;

        case Opcode::OSC_SQR:
            op_osc_sqr(ctx_, inst);
            break;

        case Opcode::OSC_RAMP:
            op_osc_ramp(ctx_, inst);
            break;

        case Opcode::OSC_PHASOR:
            op_osc_phasor(ctx_, inst);
            break;

#ifndef CEDAR_NO_MINBLEP
        case Opcode::OSC_SQR_MINBLEP:
            op_osc_sqr_minblep(ctx_, inst);
            break;
#endif

        // === PWM Oscillators ===
        case Opcode::OSC_SQR_PWM:
            op_osc_sqr_pwm(ctx_, inst);
            break;

        case Opcode::OSC_SAW_PWM:
            op_osc_saw_pwm(ctx_, inst);
            break;

#ifndef CEDAR_NO_MINBLEP
        case Opcode::OSC_SQR_PWM_MINBLEP:
            op_osc_sqr_pwm_minblep(ctx_, inst);
            break;
#endif

        // === Oversampled Oscillators (4x only, 2x variants removed) ===
        case Opcode::OSC_SIN_4X:
            op_osc_sin_4x(ctx_, inst);
            break;

        case Opcode::OSC_SAW_4X:
            op_osc_saw_4x(ctx_, inst);
            break;

        case Opcode::OSC_SQR_4X:
            op_osc_sqr_4x(ctx_, inst);
            break;

        case Opcode::OSC_TRI_4X:
            op_osc_tri_4x(ctx_, inst);
            break;

        case Opcode::OSC_SQR_PWM_4X:
            op_osc_sqr_pwm_4x(ctx_, inst);
            break;

        case Opcode::OSC_SAW_PWM_4X:
            op_osc_saw_pwm_4x(ctx_, inst);
            break;

        // === Wavetable Oscillator ===
        case Opcode::OSC_WAVETABLE:
            op_osc_wavetable(ctx_, inst);
            break;

        // === Filters (SVF only) ===
        [[likely]] case Opcode::FILTER_SVF_LP:
            op_filter_svf_lp(ctx_, inst);
            break;

        case Opcode::FILTER_SVF_HP:
            op_filter_svf_hp(ctx_, inst);
            break;

        case Opcode::FILTER_SVF_BP:
            op_filter_svf_bp(ctx_, inst);
            break;

        case Opcode::FILTER_MOOG:
            op_filter_moog(ctx_, inst);
            break;

        case Opcode::FILTER_DIODE:
            op_filter_diode(ctx_, inst);
            break;

        case Opcode::FILTER_FORMANT:
            op_filter_formant(ctx_, inst);
            break;

        case Opcode::FILTER_SALLENKEY:
            op_filter_sallenkey(ctx_, inst);
            break;

        // === Math ===
        case Opcode::ABS:
            op_abs(ctx_, inst);
            break;

        case Opcode::SQRT:
            op_sqrt(ctx_, inst);
            break;

        case Opcode::LOG:
            op_log(ctx_, inst);
            break;

        case Opcode::EXP:
            op_exp(ctx_, inst);
            break;

        case Opcode::MIN:
            op_min(ctx_, inst);
            break;

        case Opcode::MAX:
            op_max(ctx_, inst);
            break;

        case Opcode::CLAMP:
            op_clamp(ctx_, inst);
            break;

        case Opcode::WRAP:
            op_wrap(ctx_, inst);
            break;

        case Opcode::FLOOR:
            op_floor(ctx_, inst);
            break;

        case Opcode::CEIL:
            op_ceil(ctx_, inst);
            break;

        case Opcode::FMOD:
            op_fmod(ctx_, inst);
            break;

        case Opcode::EVENT_RATE_SCALE:
            op_event_rate_scale(ctx_, inst);
            break;

        case Opcode::EVENT_REORDER:
            op_event_reorder(ctx_, inst);
            break;

        case Opcode::EVENT_FANOUT:
            op_event_fanout(ctx_, inst);
            break;

        // === Trigonometric Math ===
        case Opcode::MATH_SIN:
            op_math_sin(ctx_, inst);
            break;

        case Opcode::MATH_COS:
            op_math_cos(ctx_, inst);
            break;

        case Opcode::MATH_TAN:
            op_math_tan(ctx_, inst);
            break;

        case Opcode::MATH_ASIN:
            op_math_asin(ctx_, inst);
            break;

        case Opcode::MATH_ACOS:
            op_math_acos(ctx_, inst);
            break;

        case Opcode::MATH_ATAN:
            op_math_atan(ctx_, inst);
            break;

        case Opcode::MATH_ATAN2:
            op_math_atan2(ctx_, inst);
            break;

        // === Hyperbolic Math ===
        case Opcode::MATH_SINH:
            op_math_sinh(ctx_, inst);
            break;

        case Opcode::MATH_COSH:
            op_math_cosh(ctx_, inst);
            break;

        case Opcode::MATH_TANH:
            op_math_tanh(ctx_, inst);
            break;

        // === Utility ===
        [[likely]] case Opcode::OUTPUT:
            op_output(ctx_, inst);
            break;

        case Opcode::NOISE:
            op_noise(ctx_, inst);
            break;

        case Opcode::MTOF:
            op_mtof(ctx_, inst);
            break;

        case Opcode::DC:
            op_dc(ctx_, inst);
            break;

        case Opcode::SLEW:
            op_slew(ctx_, inst);
            break;

        case Opcode::INTERP_TIME:
            op_interp_time(ctx_, inst);
            break;

        case Opcode::EDGE_OP:
            op_edge(ctx_, inst);
            break;

        case Opcode::ENV_GET:
            op_env_get(ctx_, inst);
            break;

        case Opcode::STATE_OP:
            op_state(ctx_, inst);
            break;

        case Opcode::INPUT:
            op_input(ctx_, inst);
            break;

        // === Sequencing & Timing ===
        case Opcode::CLOCK:
            op_clock(ctx_, inst);
            break;

        [[likely]] case Opcode::LFO:
            op_lfo(ctx_, inst);
            break;

        case Opcode::EUCLID:
            op_euclid(ctx_, inst);
            break;

        case Opcode::TRIGGER:
            op_trigger(ctx_, inst);
            break;

        case Opcode::TIMELINE:
            op_timeline(ctx_, inst);
            break;

        // === Runtime MIDI event source ===
        case Opcode::MIDI_QUERY:
            op_midi_query(ctx_, inst);
            break;

        // === Lazy Queryable Patterns ===
        case Opcode::SEQPAT_QUERY:
            op_seqpat_query(ctx_, inst);
            break;

        case Opcode::SEQPAT_STEP:
            op_seqpat_step(ctx_, inst);
            break;

        case Opcode::SEQPAT_GATE:
            op_seqpat_gate(ctx_, inst);
            break;

        case Opcode::SEQPAT_TYPE:
            op_seqpat_type(ctx_, inst);
            break;

        case Opcode::SEQPAT_TRANSPORT:
            op_seqpat_transport(ctx_, inst);
            break;

        case Opcode::SEQPAT_PROP:
            op_seqpat_prop(ctx_, inst);
            break;

        case Opcode::SEQPAT_FIELD:
            op_seqpat_field(ctx_, inst);
            break;

        case Opcode::SEQPAT_PHASE:
            op_seqpat_phase(ctx_, inst);
            break;

        case Opcode::SEQPAT_VALUES:
            op_seqpat_values(ctx_, inst);
            break;

        // EVENT_MAP / EVENT_FILTER are dispatched in execute_program (the
        // closure form needs slot->blocks[]); they never reach execute().

        // === Envelopes ===
        case Opcode::ENV_ADSR:
            op_env_adsr(ctx_, inst);
            break;

        case Opcode::ENV_AR:
            op_env_ar(ctx_, inst);
            break;

        case Opcode::ENV_FOLLOWER:
            op_env_follower(ctx_, inst);
            break;

        // === Samplers ===
        case Opcode::SAMPLE_PLAY:
            op_sample_play(ctx_, inst, &sample_bank_);
            break;

        case Opcode::SAMPLE_PLAY_LOOP:
            op_sample_play_loop(ctx_, inst, &sample_bank_);
            break;

#ifndef CEDAR_NO_SOUNDFONT
        case Opcode::SOUNDFONT_VOICE:
            op_soundfont_voice(ctx_, inst, &sample_bank_, &soundfont_registry_);
            break;

        case Opcode::SF_VOICE:
            op_sf_voice(ctx_, inst, &sample_bank_, &soundfont_registry_);
            break;
#endif

        // === Delays ===
        case Opcode::DELAY:
            op_delay(ctx_, inst);
            break;

        case Opcode::DELAY_TAP:
            op_delay_tap(ctx_, inst);
            break;

        case Opcode::DELAY_WRITE:
            op_delay_write(ctx_, inst);
            break;

        // === Reverbs ===
        case Opcode::REVERB_FREEVERB:
            op_reverb_freeverb(ctx_, inst);
            break;

        case Opcode::REVERB_DATTORRO:
            op_reverb_dattorro(ctx_, inst);
            break;

        case Opcode::REVERB_FDN:
            op_reverb_fdn(ctx_, inst);
            break;

        // === Modulation Effects ===
        case Opcode::EFFECT_CHORUS:
            op_effect_chorus(ctx_, inst);
            break;

        case Opcode::EFFECT_FLANGER:
            op_effect_flanger(ctx_, inst);
            break;

        case Opcode::EFFECT_PHASER:
            op_effect_phaser(ctx_, inst);
            break;

        case Opcode::EFFECT_COMB:
            op_effect_comb(ctx_, inst);
            break;

        // === Distortion ===
        case Opcode::DISTORT_TANH:
            op_distort_tanh(ctx_, inst);
            break;

        case Opcode::DISTORT_SOFT:
            op_distort_soft(ctx_, inst);
            break;

        case Opcode::DISTORT_BITCRUSH:
            op_distort_bitcrush(ctx_, inst);
            break;

        case Opcode::DISTORT_FOLD:
            op_distort_fold(ctx_, inst);
            break;

        case Opcode::DISTORT_TUBE:
            op_distort_tube(ctx_, inst);
            break;

        case Opcode::DISTORT_SMOOTH:
            op_distort_smooth(ctx_, inst);
            break;

        case Opcode::DISTORT_TAPE:
            op_distort_tape(ctx_, inst);
            break;

        case Opcode::DISTORT_XFMR:
            op_distort_xfmr(ctx_, inst);
            break;

        case Opcode::DISTORT_EXCITE:
            op_distort_excite(ctx_, inst);
            break;

        // === Dynamics ===
        case Opcode::DYNAMICS_COMP:
            op_dynamics_comp(ctx_, inst);
            break;

        case Opcode::DYNAMICS_LIMITER:
            op_dynamics_limiter(ctx_, inst);
            break;

        case Opcode::DYNAMICS_GATE:
            op_dynamics_gate(ctx_, inst);
            break;

        // === Logic & Conditionals ===
        case Opcode::SELECT:
            op_select(ctx_, inst);
            break;

        case Opcode::CMP_GT:
            op_cmp_gt(ctx_, inst);
            break;

        case Opcode::CMP_LT:
            op_cmp_lt(ctx_, inst);
            break;

        case Opcode::CMP_GTE:
            op_cmp_gte(ctx_, inst);
            break;

        case Opcode::CMP_LTE:
            op_cmp_lte(ctx_, inst);
            break;

        case Opcode::CMP_EQ:
            op_cmp_eq(ctx_, inst);
            break;

        case Opcode::CMP_NEQ:
            op_cmp_neq(ctx_, inst);
            break;

        case Opcode::LOGIC_AND:
            op_logic_and(ctx_, inst);
            break;

        case Opcode::LOGIC_OR:
            op_logic_or(ctx_, inst);
            break;

        case Opcode::LOGIC_NOT:
            op_logic_not(ctx_, inst);
            break;

        // === Arrays ===
        case Opcode::ARRAY_PACK:
            op_array_pack(ctx_, inst);
            break;

        case Opcode::ARRAY_INDEX:
            op_array_index(ctx_, inst);
            break;

        case Opcode::ARRAY_UNPACK:
            op_array_unpack(ctx_, inst);
            break;

        case Opcode::ARRAY_LEN:
            op_array_len(ctx_, inst);
            break;

        case Opcode::ARRAY_SLICE:
            op_array_slice(ctx_, inst);
            break;

        case Opcode::ARRAY_CONCAT:
            op_array_concat(ctx_, inst);
            break;

        case Opcode::ARRAY_PUSH:
            op_array_push(ctx_, inst);
            break;

        case Opcode::ARRAY_SUM:
            op_array_sum(ctx_, inst);
            break;

        case Opcode::ARRAY_REVERSE:
            op_array_reverse(ctx_, inst);
            break;

        case Opcode::ARRAY_FILL:
            op_array_fill(ctx_, inst);
            break;

        // === Stereo ===
        case Opcode::PAN:
            op_pan(ctx_, inst);
            break;

        case Opcode::WIDTH:
            op_width(ctx_, inst);
            break;

        case Opcode::MS_ENCODE:
            op_ms_encode(ctx_, inst);
            break;

        case Opcode::MS_DECODE:
            op_ms_decode(ctx_, inst);
            break;

        case Opcode::DELAY_PINGPONG:
            op_delay_pingpong(ctx_, inst);
            break;

        case Opcode::MONO_DOWNMIX:
            op_mono_downmix(ctx_, inst);
            break;

        case Opcode::PAN_STEREO:
            op_pan_stereo(ctx_, inst);
            break;

        // === Polyphony ===
        case Opcode::POLY_BEGIN:
        case Opcode::POLY_END:
            // Handled by execute_program's IP loop — should not reach here
            break;

        // === Visualization ===
        case Opcode::PROBE:
            op_probe(ctx_, inst);
            break;

#ifndef CEDAR_NO_FFT
        case Opcode::FFT_PROBE:
            op_fft_probe(ctx_, inst);
            break;
#endif

        // === Invalid ===
        [[unlikely]] case Opcode::INVALID:
        [[unlikely]] default:
            // Unknown opcode - skip
            break;
    }
}

// ============================================================================
// State Management
// ============================================================================

void VM::reset() {
    swap_controller_.reset();
    buffer_pool_.clear_all();
    state_pool_.reset();
    audio_arena_.reset();  // Reset arena when states are cleared
    clear_midi_sequences();  // Pointers were arena-owned; map now stale
    crossfade_state_.complete();
    ctx_.global_sample_counter = 0;
    ctx_.block_counter = 0;
}

std::int32_t VM::load_midi_file(std::string_view name,
                                const std::uint8_t* bytes,
                                std::size_t len) {
    if (name.empty() || !bytes || len == 0) return -1;
    std::string key(name);
    const auto existing = midi_sequences_.find(key);
    if (existing != midi_sequences_.end() && existing->second != nullptr) {
        return 0;  // already loaded; dedup against repeated drag-drop
    }
    MidiSequence* seq = parse_smf(bytes, len, audio_arena_);
    if (!seq) return -1;
    midi_sequences_[std::move(key)] = seq;
    return 0;
}

void VM::clear_midi_sequences() {
    // Pointer values live in audio_arena_ and are freed by arena.reset();
    // we only own the map keys.
    midi_sequences_.clear();
}

void VM::hot_swap_begin() {
    // Legacy API - begin frame clears the touched set
    state_pool_.begin_frame();
}

void VM::hot_swap_end() {
    // Legacy API - GC sweep removes states that weren't touched
    state_pool_.gc_sweep();
}

void VM::set_crossfade_blocks(std::uint32_t blocks) {
    crossfade_config_.set_duration(blocks);
    state_pool_.set_fade_blocks(blocks);
}

// ============================================================================
// Configuration
// ============================================================================

void VM::set_sample_rate(float rate) {
    ctx_.set_sample_rate(rate);
    env_map_.set_sample_rate(rate);
    env_map_.set_param("__sr", rate);
}

void VM::set_bpm(float bpm) {
    ctx_.bpm = bpm;
    env_map_.set_param("__bpm", bpm);
    env_map_.set_param("__spb", bpm > 0.0f ? 60.0f / bpm : 0.0f);
}

void VM::set_input_buffers(float* input_left, float* input_right) {
    ctx_.input_left = input_left;
    ctx_.input_right = input_right;
}

// ============================================================================
// External Parameter Binding
// ============================================================================

bool VM::set_param(const char* name, float value) {
    return env_map_.set_param(name, value);
}

bool VM::set_param(const char* name, float value, float slew_ms) {
    return env_map_.set_param(name, value, slew_ms);
}

// ============================================================================
// Sample Management
// ============================================================================

std::uint32_t VM::load_sample(const std::string& name,
                              const float* audio_data,
                              std::size_t num_samples,
                              std::uint32_t channels,
                              float sample_rate) {
    return sample_bank_.load_sample(name, audio_data, num_samples, channels, sample_rate);
}

void VM::remove_param(const char* name) {
    env_map_.remove_param(name);
}

bool VM::has_param(const char* name) const {
    return env_map_.has_param(name);
}

// ============================================================================
// Query API
// ============================================================================

bool VM::is_crossfading() const {
    return crossfade_state_.is_active();
}

float VM::crossfade_position() const {
    return crossfade_state_.position();
}

bool VM::has_program() const {
    return swap_controller_.has_program();
}

std::uint32_t VM::swap_count() const {
    return swap_controller_.swap_count();
}

bool VM::has_pending_swap() const {
    return swap_controller_.has_pending_swap();
}

std::uint32_t VM::current_slot_instruction_count() const {
    const auto* slot = swap_controller_.current_slot();
    return slot ? slot->instruction_count : 0;
}

std::uint32_t VM::previous_slot_instruction_count() const {
    const auto* slot = swap_controller_.previous_slot();
    return slot ? slot->instruction_count : 0;
}

// ============================================================================
// Timeline Seek
// ============================================================================

void VM::seek(float beat_position, const SeekConfig& config) {
    float samples_per_beat = ctx_.samples_per_beat();
    std::uint64_t target_sample = static_cast<std::uint64_t>(beat_position * samples_per_beat);
    seek_samples(target_sample, config);
}

void VM::seek_samples(std::uint64_t sample_position, const SeekConfig& config) {
    // Update global timing to target position
    ctx_.global_sample_counter = sample_position;
    ctx_.block_counter = sample_position / BLOCK_SIZE;
    ctx_.update_timing();

    // Reconstruct deterministic states (oscillator phases, LFO phases, etc.)
    // Note: This is a best-effort reconstruction assuming constant parameters.
    // For modulated parameters, the phase won't be exact.
    reconstruct_deterministic_states(sample_position);

    // Handle history-dependent states
    if (config.reset_history_dependent) {
        reset_history_dependent_states();
    }

    // Optional pre-roll to warm up filters/delays
    if (config.preroll_blocks > 0) {
        execute_preroll(config.preroll_blocks);
    }
}

float VM::current_beat_position() const {
    return static_cast<float>(ctx_.global_sample_counter) / ctx_.samples_per_beat();
}

std::uint64_t VM::current_sample_position() const {
    return ctx_.global_sample_counter;
}

void VM::reconstruct_deterministic_states([[maybe_unused]] std::uint64_t target_sample) {
    // For deterministic state reconstruction, we would iterate through all states
    // and recalculate phases based on the target sample position.
    //
    // This only works for states where the phase can be derived from time alone.
    // For modulated parameters, we use a heuristic based on typical usage.

    // Note: The current architecture doesn't store the frequency/parameters
    // alongside the state, so we can only do a partial reconstruction.
    // Full reconstruction would require storing parameter snapshots.

    // For now, we reset phases to be consistent with the target time.
    // The actual reconstruction happens when each opcode executes,
    // using the current parameters at that time.

    // Key insight: Most sequencing opcodes (LFO, Euclid, Trigger)
    // derive their phase from global_sample_counter, which we've already updated.
    // When these opcodes run, they will calculate the correct phase for the
    // new position.

    // For oscillators, we could estimate phase if we knew frequency, but since
    // frequency is typically modulated (from sequencers), exact reconstruction
    // isn't possible without full state history.

    // The pragmatic approach: oscillator phases will be "wrong" after seek,
    // but this is usually inaudible since oscillators are phase-continuous
    // and the seek point is arbitrary anyway.
}

void VM::reset_history_dependent_states() {
    // Reset all history-dependent states to their initial values.
    // This includes filters (SVF, Moog), delays, envelopes, slew, SAH.

    // Note: We can't directly iterate the state pool by type, but we can
    // rely on the state pool's internal storage. For now, we just mark
    // that a reset is needed and let opcodes handle it.

    // The cleanest approach is to reset the entire state pool, which will
    // cause all states to be recreated with default values on next access.
    // However, this loses oscillator phases too.

    // Alternative: Add a "needs_reset" flag to the context that opcodes check.
    // For simplicity, we'll do a selective reset by visiting known state types.

    // For the initial implementation, we do a full state reset.
    // This is aggressive but ensures clean state after seek.
    state_pool_.reset();
}

void VM::execute_preroll(std::uint32_t blocks) {
    // Execute program for N blocks, discarding output.
    // This warms up filters and delays with the current audio content.

    const ProgramSlot* current = swap_controller_.current_slot();
    if (!current || current->instruction_count == 0) {
        // No program to pre-roll
        ctx_.global_sample_counter += blocks * BLOCK_SIZE;
        ctx_.block_counter += blocks;
        return;
    }

    // Temporary output buffers (discarded)
    alignas(32) std::array<float, BLOCK_SIZE> temp_left{};
    alignas(32) std::array<float, BLOCK_SIZE> temp_right{};

    for (std::uint32_t i = 0; i < blocks; ++i) {
        ctx_.update_timing();
        execute_program(current, temp_left.data(), temp_right.data());
        ctx_.global_sample_counter += BLOCK_SIZE;
        ctx_.block_counter++;
    }
}

}  // namespace cedar
