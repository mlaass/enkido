#include "cedar/vm/vm.hpp"
#include "cedar/opcodes/opcodes.hpp"
#include <algorithm>

namespace cedar {

VM::VM() {
    // Initialize context with pointers to our pools
    ctx_.buffers = &buffer_pool_;
    ctx_.states = &state_pool_;
    ctx_.env_map = &env_map_;
}

VM::~VM() = default;

// ============================================================================
// Program Loading
// ============================================================================

VM::LoadResult VM::load_program(std::span<const Instruction> bytecode) {
    if (bytecode.size() > MAX_PROGRAM_SIZE) {
        return LoadResult::TooLarge;
    }

    if (!swap_controller_.load_program(bytecode)) {
        return LoadResult::SlotBusy;
    }

    return LoadResult::Success;
}

bool VM::load_program_immediate(std::span<const Instruction> bytecode) {
    // Reset everything first
    reset();

    // Load directly into current slot
    ProgramSlot* slot = swap_controller_.acquire_write_slot();
    if (!slot) return false;

    if (!slot->load(bytecode)) {
        return false;
    }

    // Submit and immediately swap
    swap_controller_.submit_ready(slot);
    swap_controller_.execute_swap();

    return true;
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
        // No program - output silence
        ctx_.global_sample_counter += BLOCK_SIZE;
        ctx_.block_counter++;
        return;
    }

    // Update timing
    ctx_.update_timing();

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
        swap_controller_.release_previous();
        crossfade_state_.complete();
        // Move orphaned states to fading pool
        state_pool_.gc_sweep();
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

    // Get old slot before swap
    const ProgramSlot* old_slot = swap_controller_.current_slot();

    // Execute the swap
    if (!swap_controller_.execute_swap()) {
        return;  // Swap failed
    }

    const ProgramSlot* new_slot = swap_controller_.current_slot();

    // Rebind states from old to new program
    rebind_states(old_slot, new_slot);

    // Determine if crossfade is needed
    if (old_slot && old_slot->instruction_count > 0 &&
        requires_crossfade(old_slot, new_slot)) {
        crossfade_state_.begin(crossfade_config_.duration_blocks);
    } else {
        // No crossfade needed - immediately release previous slot
        // This prevents slot starvation when doing rapid non-structural changes
        swap_controller_.release_previous();
    }
}

void VM::perform_crossfade(float* out_left, float* out_right) {
    // Get both program slots
    const ProgramSlot* old_slot = swap_controller_.previous_slot();
    const ProgramSlot* new_slot = swap_controller_.current_slot();

    // Execute old program into crossfade buffers
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

    // Execute new program into crossfade buffers
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

    // Quick check: identical signatures mean no structural change
    if (old_slot->signature == new_slot->signature) {
        return false;
    }

    // Check for added or removed state IDs
    auto old_ids = old_slot->get_state_ids();
    auto new_ids = new_slot->get_state_ids();

    // Count IDs in new but not in old (added nodes)
    for (auto id : new_ids) {
        if (!old_slot->has_state_id(id)) {
            return true;  // Node added
        }
    }

    // Count IDs in old but not in new (removed nodes)
    for (auto id : old_ids) {
        if (!new_slot->has_state_id(id)) {
            return true;  // Node removed
        }
    }

    // No structural changes
    return false;
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

void VM::execute_program(const ProgramSlot* slot, float* out_left, float* out_right) {
    // Set output buffer pointers
    ctx_.output_left = out_left;
    ctx_.output_right = out_right;

    // Mark beginning of frame for state GC tracking
    state_pool_.begin_frame();

    // Execute all instructions
    auto program = slot->program();
    for (const auto& inst : program) {
        execute(inst);
    }
}

void VM::execute(const Instruction& inst) {
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

        case Opcode::SAH:
            op_sah(ctx_, inst);
            break;

        case Opcode::ENV_GET:
            op_env_get(ctx_, inst);
            break;

        // === Sequencing & Timing ===
        case Opcode::CLOCK:
            op_clock(ctx_, inst);
            break;

        [[likely]] case Opcode::LFO:
            op_lfo(ctx_, inst);
            break;

        case Opcode::SEQ_STEP:
            op_seq_step(ctx_, inst);
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

        // === Reserved/Invalid ===
        [[unlikely]] case Opcode::ENV_ADSR:
        [[unlikely]] case Opcode::ENV_AR:
        [[unlikely]] case Opcode::DELAY:
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
    crossfade_state_.complete();
    ctx_.global_sample_counter = 0;
    ctx_.block_counter = 0;
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
}

void VM::set_bpm(float bpm) {
    ctx_.bpm = bpm;
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

}  // namespace cedar
