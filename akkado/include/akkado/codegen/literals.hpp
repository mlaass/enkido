#pragma once

// Inline helpers for literal code generation
// These provide common patterns used in NumberLit, BoolLit, PitchLit, ChordLit, ArrayLit

#include "helpers.hpp"
#include <cedar/vm/instruction.hpp>

namespace akkado {
namespace codegen {

/// Emit PUSH_CONST for a MIDI note value, then MTOF to convert to frequency.
/// This is the common pattern used by PitchLit and ChordLit.
/// Returns the frequency buffer index, or BUFFER_UNUSED on error.
[[gnu::always_inline]]
inline std::uint16_t emit_midi_to_freq(
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions,
    float midi_note
) {
    // Allocate buffer for MIDI value
    std::uint16_t midi_buf = buffers.allocate();
    if (midi_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    // Push MIDI note value
    cedar::Instruction push_inst{};
    push_inst.opcode = cedar::Opcode::PUSH_CONST;
    push_inst.out_buffer = midi_buf;
    push_inst.inputs[0] = 0xFFFF;
    push_inst.inputs[1] = 0xFFFF;
    push_inst.inputs[2] = 0xFFFF;
    push_inst.inputs[3] = 0xFFFF;
    encode_const_value(push_inst, midi_note);
    instructions.push_back(push_inst);

    // Allocate buffer for frequency
    std::uint16_t freq_buf = buffers.allocate();
    if (freq_buf == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    // MTOF: convert MIDI note to frequency
    cedar::Instruction mtof_inst{};
    mtof_inst.opcode = cedar::Opcode::MTOF;
    mtof_inst.out_buffer = freq_buf;
    mtof_inst.inputs[0] = midi_buf;
    mtof_inst.inputs[1] = 0xFFFF;
    mtof_inst.inputs[2] = 0xFFFF;
    mtof_inst.inputs[3] = 0xFFFF;
    mtof_inst.state_id = 0;
    instructions.push_back(mtof_inst);

    return freq_buf;
}

/// Create a simple PUSH_CONST instruction object (not emitted).
/// Caller is responsible for emitting and tracking the buffer.
[[gnu::always_inline]]
inline cedar::Instruction make_push_const(std::uint16_t out_buffer, float value) {
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::PUSH_CONST;
    inst.out_buffer = out_buffer;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    encode_const_value(inst, value);
    return inst;
}

/// Create a MTOF instruction object (not emitted).
[[gnu::always_inline]]
inline cedar::Instruction make_mtof(std::uint16_t out_buffer, std::uint16_t midi_input) {
    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::MTOF;
    inst.out_buffer = out_buffer;
    inst.inputs[0] = midi_input;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.state_id = 0;
    return inst;
}

} // namespace codegen
} // namespace akkado
