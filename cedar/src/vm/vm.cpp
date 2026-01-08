#include "cedar/vm/vm.hpp"
#include "cedar/opcodes/opcodes.hpp"
#include <algorithm>

namespace cedar {

VM::VM() {
    // Initialize context with pointers to our pools
    ctx_.buffers = &buffer_pool_;
    ctx_.states = &state_pool_;
}

VM::~VM() = default;

bool VM::load_program(std::span<const Instruction> bytecode) {
    if (bytecode.size() > MAX_PROGRAM_SIZE) {
        return false;
    }
    program_.assign(bytecode.begin(), bytecode.end());
    return true;
}

void VM::process_block(float* output_left, float* output_right) {
    // Set output buffer pointers
    ctx_.output_left = output_left;
    ctx_.output_right = output_right;

    // Clear output buffers
    std::fill_n(output_left, BLOCK_SIZE, 0.0f);
    std::fill_n(output_right, BLOCK_SIZE, 0.0f);

    // Update timing
    ctx_.update_timing();

    // Mark beginning of frame for state GC tracking
    state_pool_.begin_frame();

    // Execute all instructions
    for (const auto& inst : program_) {
        execute(inst);
    }

    // Advance timing
    ctx_.global_sample_counter += BLOCK_SIZE;
    ctx_.block_counter++;
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

        // === Filters ===
        [[likely]] case Opcode::FILTER_LP:
            op_filter_lp(ctx_, inst);
            break;

        case Opcode::FILTER_HP:
            op_filter_hp(ctx_, inst);
            break;

        case Opcode::FILTER_BP:
            op_filter_bp(ctx_, inst);
            break;

        case Opcode::FILTER_SVF_LP:
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

void VM::reset() {
    program_.clear();
    buffer_pool_.clear_all();
    state_pool_.reset();
    ctx_.global_sample_counter = 0;
    ctx_.block_counter = 0;
}

void VM::hot_swap_begin() {
    // Begin frame clears the touched set
    state_pool_.begin_frame();
}

void VM::hot_swap_end() {
    // GC sweep removes states that weren't touched
    state_pool_.gc_sweep();
}

void VM::set_sample_rate(float rate) {
    ctx_.set_sample_rate(rate);
}

void VM::set_bpm(float bpm) {
    ctx_.bpm = bpm;
}

}  // namespace cedar
