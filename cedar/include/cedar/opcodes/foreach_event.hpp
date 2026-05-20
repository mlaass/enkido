#pragma once

#include "../vm/instruction.hpp"
#include <cstdint>

// FOREACH_EVENT — PRD prd-runtime-functions-control-flow L3.
//
// FOREACH_EVENT generalizes the POLY_BEGIN/POLY_END special case into a uniform
// "invoke a subprogram body once per event/voice" opcode. Like POLY_BEGIN and
// the L1 control-flow opcodes it is a *dispatch-loop* opcode: the VM handles it
// directly in VM::execute_program, NOT through the execute() opcode switch.
//
// Unlike POLY_BEGIN, the block body is NOT inline after the opcode — it lives
// in the ProgramSlot subprogram table (ProgramSlot::blocks[]). The instruction
// immediately after FOREACH_EVENT is the normal main-program continuation, so
// the dispatch loop advances by exactly one (no body/terminator to skip).
//
// Instruction field usage:
//   opcode     = FOREACH_EVENT
//   state_id   = identifies this FOREACH instance (one per call site)
//   out_buffer = mix bus L (R derived +1 for stereo output)
//   inputs[0..4] = convention slots. VOICE_POOL: freq/gate/vel/trig/voice_out
//                  (identical to POLY_BEGIN). PER_ITERATION/SHARED: field
//                  buffers + output slot per the FrameDescriptor.
//   rate       = 0 (body length is NOT carried here — body is in the table).
//   flags      = STEREO_OUTPUT when the body produces stereo.
//
// block_id and allocator configuration travel out-of-band in the matching
// StateInitData::ForeachAlloc entry (keyed by state_id) and are applied by the
// host via VM::init_foreach_state before the first process_block.

namespace cedar {

// Allocator kinds for FOREACH_EVENT. A compile-time enum; the runtime reads it
// from the resolved foreach state (set at init_foreach_state time).
enum class ForeachAllocKind : std::uint8_t {
    VoicePool    = 0,  // POLY semantics: gate-on allocates, gate-off releases,
                       // events overlap. Backed by PolyAllocState.
    PerIteration = 1,  // each event gets a fresh per-iteration state slice,
                       // re-derived every block. Backed by ForeachIterState.
    Shared       = 2,  // all iterations share one state slot (fold accumulator).
                       // Backed by ForeachSharedState.
};

}  // namespace cedar
