#pragma once

#include "../vm/instruction.hpp"
#include <cstddef>

// Forward control-flow opcodes — PRD prd-runtime-functions-control-flow L1.
//
// Unlike the data-transform opcodes in the other headers, the control-flow
// opcodes (SKIP_IF_ZERO / SKIP_IF_NONZERO) are NOT dispatched through the
// execute() opcode switch. They alter the instruction pointer and are handled
// directly in the VM dispatch loop (VM::execute_program), the same way
// POLY_BEGIN is. They are forward-only: they advance `ip`, never rewind it,
// so the instruction stream stays acyclic.
//
// Encoding: inputs[0] is the predicate buffer; the predicate is sampled once
// per block at sample [0]. `rate` (uint8) is the number of instructions to
// skip when the branch is taken.

namespace cedar {

// Compute the next instruction pointer for a SKIP_IF_* opcode.
//   predicate — first sample of the inputs[0] buffer.
//   ip        — index of the SKIP_IF_* instruction itself.
// When the skip is taken, advance past `rate` instructions plus the SKIP
// itself (ip += rate + 1); otherwise fall through to the next instruction.
[[nodiscard]] inline std::size_t skip_if_next_ip(const Instruction& inst,
                                                 float predicate,
                                                 std::size_t ip) noexcept {
    const bool taken = (inst.opcode == Opcode::SKIP_IF_ZERO)
                           ? (predicate == 0.0f)
                           : (predicate != 0.0f);
    return ip + (taken ? (static_cast<std::size_t>(inst.rate) + 1) : 1);
}

}  // namespace cedar
