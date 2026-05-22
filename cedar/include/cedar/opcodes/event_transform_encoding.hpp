#pragma once

#include <cstdint>

// Instruction-encoding constants shared by the EVENT_MAP / EVENT_FILTER
// runtime opcode bodies (cedar/opcodes/event_transforms.hpp) and the Akkado
// codegen that emits them. Kept dependency-free so the compiler can include
// it without pulling in the VM execution headers.
//
// PRD prd-runtime-event-transforms.md, Phase 1. The `rate` byte packs a
// field selector (bits 0-3) and an op / comparison (bits 4-5).

namespace cedar {

// Field selectors (rate bits 0-3).
enum : std::uint8_t {
    EVENT_FIELD_NOTE_COUPLED = 0,  // EVENT_MAP only: transpose — shift midi_note
                                   // + per-voice notes[], scale values[] (Hz).
    EVENT_FIELD_VEL          = 1,
    EVENT_FIELD_DUR          = 2,
    EVENT_FIELD_TIME         = 3,
    EVENT_FIELD_CHANCE       = 4,
};

// EVENT_MAP ops (rate bits 4-5).
enum : std::uint8_t {
    EVENT_OP_ADD = 0,
    EVENT_OP_MUL = 1,
    EVENT_OP_SET = 2,
};

// EVENT_FILTER comparisons (rate bits 4-5).
enum : std::uint8_t {
    EVENT_CMP_GTE = 0,  // keep events whose field >= threshold
    EVENT_CMP_LTE = 1,  // keep events whose field <= threshold
};

// Pack a field selector + op/comparison into the instruction rate byte.
[[nodiscard]] constexpr std::uint8_t event_transform_rate(std::uint8_t field,
                                                          std::uint8_t op) noexcept {
    return static_cast<std::uint8_t>((field & 0x0F) | ((op & 0x03) << 4));
}

// ----------------------------------------------------------------------------
// Phase 2 — closure EVENT_MAP / EVENT_FILTER (InstructionFlag::EVENT_CLOSURE).
// ----------------------------------------------------------------------------
//
// A closure EVENT_MAP / EVENT_FILTER instruction encodes:
//   flags:      EVENT_CLOSURE | (write-mask << EVENT_MASK_SHIFT)
//   rate:       closure subprogram block_id
//   inputs[0]:  primary (freq) input buffer for the closure's event record
//   inputs[1]:  input event-record bank base (vel,dur,note,chance,time,gate,
//               trig — the FOREACH bank layout), or 0xFFFF if unused
//   inputs[2/3]:low / high 16 bits of the upstream state_id
//   inputs[4]:  EVENT_MAP — output field-bank base (7 contiguous buffers,
//               EVENT_OUT_* layout); EVENT_FILTER — predicate result buffer
//   out_buffer: 0xFFFF
//   state_id:   the transform-owned downstream SequenceState
//
// The closure body writes the assigned EVENT_OUT_* slots (EVENT_MAP) or the
// predicate buffer (EVENT_FILTER); the opcode overlays / filters per event.

// Output field-bank slots for a closure EVENT_MAP. The closure's returned
// record maps each field name to one of these; the write mask records which
// slots were assigned so unset fields pass through unchanged (shallow overlay).
enum : std::uint8_t {
    EVENT_OUT_NOTE   = 0,  // absolute MIDI note; coupled rewrite of notes[]/values[]
    EVENT_OUT_VEL    = 1,
    EVENT_OUT_DUR    = 2,
    EVENT_OUT_TIME   = 3,
    EVENT_OUT_CHANCE = 4,
    EVENT_OUT_BEND   = 5,  // custom prop slot 0
    EVENT_OUT_AT     = 6,  // custom prop slot 1
    EVENT_OUT_COUNT  = 7,
};

}  // namespace cedar
