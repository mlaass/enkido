#include "cedar/vm/vm.hpp"
#include "cedar/io/midi_sequence.hpp"
#include "cedar/opcodes/opcodes.hpp"
#include <algorithm>
#include <array>
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

    // Execute all instructions (index-based for POLY block jumping)
    auto program = slot->program();
    std::size_t ip = 0;
    while (ip < program.size()) {
        if (program[ip].opcode == Opcode::POLY_BEGIN) {
            ip = execute_poly_block(program, ip);
        } else {
            execute(program[ip]);
            ++ip;
        }
    }
}

std::size_t VM::execute_poly_block(std::span<const Instruction> program, std::size_t ip) {
    const auto& poly_inst = program[ip];
    std::uint8_t body_length = poly_inst.rate;
    std::uint16_t mix_buf = poly_inst.out_buffer;
    std::uint16_t voice_freq_buf = poly_inst.inputs[0];
    std::uint16_t voice_gate_buf = poly_inst.inputs[1];
    std::uint16_t voice_vel_buf = poly_inst.inputs[2];
    std::uint16_t voice_trig_buf = poly_inst.inputs[3];
    std::uint16_t voice_out_buf = poly_inst.inputs[4];

    // poly is stereo-native: voice_out and mix are adjacent L/R pairs. POLY_BEGIN
    // has no free input slots, so the R buffers are derived via the +1 adjacency
    // convention (codegen guarantees the pairs are adjacent).
    std::uint16_t mix_buf_r = mix_buf + 1;
    std::uint16_t voice_out_buf_r = voice_out_buf + 1;

    // Get PolyAllocState
    auto& poly_state = state_pool_.get_or_create<PolyAllocState>(poly_inst.state_id);
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
        // Cycle-position snap tolerance, in beats. IEEE-754 division of large
        // sample counters by spb produces tiny non-zero fmod residues at exact
        // cycle boundaries (e.g., 5.7e-14 at block 99000 / cycle 121 with
        // BPM 110). Those residues are far below 1-sample resolution
        // (1 sample ≈ 3.8e-5 beats at 110 BPM / 48 kHz) but break strict
        // `evt_start >= cycle_pos` comparisons against zero-time events.
        // Snap cycle_pos to 0 (and block_end to cycle_length) within an
        // epsilon well below sample resolution but well above fp noise.
        constexpr double CYCLE_BOUNDARY_EPSILON = 1e-9;
#ifdef CEDAR_FLOAT_ONLY
        // Float-only beat timing (precision degrades after ~6 min at 48kHz)
        const float spb = (60.0f / ctx_.bpm) * ctx_.sample_rate;
        const float beat_start = static_cast<float>(ctx_.global_sample_counter) / spb;
        float cycle_pos_raw = std::fmod(beat_start, cycle_length);
        if (cycle_pos_raw < static_cast<float>(CYCLE_BOUNDARY_EPSILON)) cycle_pos_raw = 0.0f;
        const float cycle_pos = cycle_pos_raw;
        const std::uint32_t current_cycle =
            static_cast<std::uint32_t>(std::floor(beat_start / cycle_length));
        float block_end_pos_raw = cycle_pos + static_cast<float>(BLOCK_SIZE) / spb;
        if (std::abs(block_end_pos_raw - cycle_length) < static_cast<float>(CYCLE_BOUNDARY_EPSILON)) {
            block_end_pos_raw = cycle_length;
        }
        const float block_end_pos = block_end_pos_raw;
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
        const std::uint32_t current_cycle =
            static_cast<std::uint32_t>(std::floor(beat_start_d / cycle_length));
        const double block_beats_d = static_cast<double>(BLOCK_SIZE) / spb_d;
        double block_end_pos_d_raw = cycle_pos_d + block_beats_d;
        if (std::abs(block_end_pos_d_raw - static_cast<double>(cycle_length))
                < CYCLE_BOUNDARY_EPSILON) {
            block_end_pos_d_raw = static_cast<double>(cycle_length);
        }
        const double block_end_pos_d = block_end_pos_d_raw;
        // Narrow to float for comparisons against float-precision event times
        const float spb = static_cast<float>(spb_d);
        const float cycle_pos = static_cast<float>(cycle_pos_d);
        const float block_end_pos = static_cast<float>(block_end_pos_d);
#endif

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

                // Allocate a voice for each chord note in this event
                for (std::uint8_t vi = 0; vi < evt.num_values; ++vi) {
                    poly_state.allocate_voice(
                        evt.values[vi], evt.velocity,
                        static_cast<std::uint16_t>(e), on_cycle, on_sample);
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

        // Fill voice parameter buffers
        float* freq_buf = buffer_pool_.get(voice_freq_buf);
        float* gate_buf = buffer_pool_.get(voice_gate_buf);
        float* vel_buf = buffer_pool_.get(voice_vel_buf);
        float* trig_buf = buffer_pool_.get(voice_trig_buf);

        std::fill_n(freq_buf, BLOCK_SIZE, voice.freq);
        std::fill_n(vel_buf, BLOCK_SIZE, voice.vel);
        std::fill_n(trig_buf, BLOCK_SIZE, 0.0f);

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

        // Execute body instructions
        for (std::size_t bi = 0; bi < body_length; ++bi) {
            execute(program[ip + 1 + bi]);
        }

        // Accumulate voice output into the stereo mix, gated by the (mono)
        // gate signal applied identically to both channels.
        const float* voice_out = buffer_pool_.get(voice_out_buf);
        const float* voice_out_r = buffer_pool_.get(voice_out_buf_r);
        const float* gate = buffer_pool_.get(voice_gate_buf);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            const float g = gate[i];
            mix[i]   += voice_out[i]   * g;
            mix_r[i] += voice_out_r[i] * g;
        }
    }

    // Reset XOR
    state_pool_.set_state_id_xor(0);

    // Advance past POLY_BEGIN + body + POLY_END
    return ip + 1 + body_length + 1;
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
