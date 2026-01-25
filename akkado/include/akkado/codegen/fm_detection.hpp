#pragma once

// FM Detection helpers for automatic oscillator upgrade
// When an oscillator's frequency input comes from an audio-rate source,
// we upgrade it to 4x oversampled variant to reduce aliasing.

#include <cedar/vm/instruction.hpp>

namespace akkado {
namespace codegen {

/// Check if opcode produces an audio-rate signal (oscillators, noise)
[[gnu::always_inline]]
inline bool is_audio_rate_producer(cedar::Opcode op) {
    switch (op) {
        // All oscillators produce audio-rate signals
        case cedar::Opcode::OSC_SIN:
        case cedar::Opcode::OSC_SIN_2X:
        case cedar::Opcode::OSC_SIN_4X:
        case cedar::Opcode::OSC_TRI:
        case cedar::Opcode::OSC_TRI_2X:
        case cedar::Opcode::OSC_TRI_4X:
        case cedar::Opcode::OSC_SAW:
        case cedar::Opcode::OSC_SAW_2X:
        case cedar::Opcode::OSC_SAW_4X:
        case cedar::Opcode::OSC_SQR:
        case cedar::Opcode::OSC_SQR_2X:
        case cedar::Opcode::OSC_SQR_4X:
        case cedar::Opcode::OSC_RAMP:
        case cedar::Opcode::OSC_PHASOR:
        case cedar::Opcode::OSC_SQR_MINBLEP:
        case cedar::Opcode::OSC_SQR_PWM:
        case cedar::Opcode::OSC_SAW_PWM:
        case cedar::Opcode::OSC_SQR_PWM_MINBLEP:
        case cedar::Opcode::OSC_SQR_PWM_4X:
        case cedar::Opcode::OSC_SAW_PWM_4X:
        case cedar::Opcode::NOISE:
            return true;
        default:
            return false;
    }
}

/// Check if opcode is a basic oscillator that can be upgraded to 4x
[[gnu::always_inline]]
inline bool is_upgradeable_oscillator(cedar::Opcode op) {
    switch (op) {
        case cedar::Opcode::OSC_SIN:
        case cedar::Opcode::OSC_TRI:
        case cedar::Opcode::OSC_SAW:
        case cedar::Opcode::OSC_SQR:
        case cedar::Opcode::OSC_SQR_PWM:
        case cedar::Opcode::OSC_SAW_PWM:
            return true;
        default:
            return false;
    }
}

/// Upgrade basic oscillator opcode to 4x oversampled variant
[[gnu::always_inline]]
inline cedar::Opcode upgrade_for_fm(cedar::Opcode op) {
    switch (op) {
        case cedar::Opcode::OSC_SIN: return cedar::Opcode::OSC_SIN_4X;
        case cedar::Opcode::OSC_TRI: return cedar::Opcode::OSC_TRI_4X;
        case cedar::Opcode::OSC_SAW: return cedar::Opcode::OSC_SAW_4X;
        case cedar::Opcode::OSC_SQR: return cedar::Opcode::OSC_SQR_4X;
        case cedar::Opcode::OSC_SQR_PWM: return cedar::Opcode::OSC_SQR_PWM_4X;
        case cedar::Opcode::OSC_SAW_PWM: return cedar::Opcode::OSC_SAW_PWM_4X;
        default: return op;  // No upgrade available
    }
}

} // namespace codegen
} // namespace akkado
