#pragma once

// Inline helpers for literal code generation
// These provide common patterns used in NumberLit, BoolLit, PitchLit, ArrayLit

#include "helpers.hpp"
#include <cedar/vm/instruction.hpp>

namespace akkado {
namespace codegen {

// NOTE: The free `emit_midi_to_freq(buffers, instructions, midi)` helper
// was removed in PRD prd-parser-codegen-correctness.md Phase 3 (F2). Use
// `CodeGenerator::emit_midi_to_freq(midi_note)` instead — it routes both
// the PUSH_CONST and MTOF emits through emit() so source_locations_ stays
// in sync.

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
