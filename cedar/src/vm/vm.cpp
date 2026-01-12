#include "cedar/vm/vm.hpp"
#include "cedar/opcodes/opcodes.hpp"
#include <algorithm>
#include <array>

namespace cedar {

VM::VM() {
    // Initialize context with pointers to our pools
    ctx_.buffers = &buffer_pool_;
    ctx_.states = &state_pool_;
    ctx_.arena = &audio_arena_;
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
                           [[maybe_unused]] const ProgramSlot* new_slot) const {
    if (!old_slot || old_slot->instruction_count == 0) {
        // First program load - no crossfade needed
        return false;
    }

    // Always crossfade when replacing an existing program
    // The signature-based detection misses changes to stateless instructions
    // (arithmetic, routing, output) which can cause audible pops
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

        case Opcode::OSC_SQR_MINBLEP:
            op_osc_sqr_minblep(ctx_, inst);
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

        // === Delays ===
        case Opcode::DELAY:
            op_delay(ctx_, inst);
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

    // Key insight: Most sequencing opcodes (LFO, SeqStep, Euclid, Trigger)
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
