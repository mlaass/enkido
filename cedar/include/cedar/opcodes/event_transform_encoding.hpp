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

// Input event-record bank layout for closure EVENT_MAP / EVENT_FILTER /
// FOREACH_EVENT (per `inst.inputs[1]`). Slots 0..6 are scalar fields populated
// by `fill_event_record_bank` (vel/dur/note/chance/time/gate/trig). Phase 5
// Commit D adds chord-array slots 7..9 so closure bodies can read
// `e.notes` / `e.freqs` as DynArrays.
enum : std::uint8_t {
    EVENT_BANK_VEL          = 0,
    EVENT_BANK_DUR          = 1,
    EVENT_BANK_NOTE         = 2,
    EVENT_BANK_CHANCE       = 3,
    EVENT_BANK_TIME         = 4,
    EVENT_BANK_GATE         = 5,
    EVENT_BANK_TRIG         = 6,
    EVENT_BANK_NOTES_DATA   = 7,  // chord notes[k] (MIDI) packed in samples 0..num_values-1
    EVENT_BANK_FREQS_DATA   = 8,  // chord values[k] (Hz)  packed in samples 0..num_values-1
    EVENT_BANK_NUM_VALUES   = 9,  // num_values broadcast across the block
    EVENT_BANK_COUNT        = 10,
};

// ----------------------------------------------------------------------------
// Phase 4 — EVENT_REORDER / EVENT_FANOUT kind selectors
// ----------------------------------------------------------------------------
//
// Each opcode body switches on `inst.rate & 0x0F`. Bits 4-7 are per-kind flags
// (ITER direction; reserved otherwise). Param-slot encoding per kind is
// documented in cedar/vm/instruction.hpp on the opcode declarations.

// EVENT_REORDER kinds (rate bits 0-3).
enum : std::uint8_t {
    EVENT_REORDER_REV        = 0,
    EVENT_REORDER_PALINDROME = 1,
    EVENT_REORDER_ITER       = 2,  // rate bit 4 = dir (0 = +1).
                                   // inputs[0]: const buf holding n (>= 1).
    EVENT_REORDER_ITER_BACK  = 3,  // dispatch shares code with ITER keyed on
                                   // dir; alias for diagnostic clarity.
    EVENT_REORDER_ZOOM       = 4,  // inputs[0]=start, inputs[1]=end (signal ok).
    EVENT_REORDER_COMPRESS   = 5,  // inputs[0]=start, inputs[1]=end (signal ok).
};

// EVENT_FANOUT kinds (rate bits 0-3).
enum : std::uint8_t {
    EVENT_FANOUT_PLY     = 0,  // inputs[0] = const buf holding n (>= 1).
    EVENT_FANOUT_LINGER  = 1,  // inputs[0] = frac (signal ok); clamped to (0,1].
    EVENT_FANOUT_SEGMENT = 2,  // inputs[0] = const buf holding n (>= 1).
};

// ITER direction packed into the rate-byte high nibble for EVENT_REORDER.
// bit 4 = 0 -> dir = +1 (iter forward); bit 4 = 1 -> dir = -1 (iterBack).
constexpr std::uint8_t EVENT_REORDER_ITER_DIR_BACK = 1u << 0;

// Pack/unpack helpers. Mirrors event_transform_rate() above.
[[nodiscard]] constexpr std::uint8_t event_reorder_rate(std::uint8_t kind,
                                                        std::uint8_t flags = 0) noexcept {
    return static_cast<std::uint8_t>((kind & 0x0F) | ((flags & 0x0F) << 4));
}
[[nodiscard]] constexpr std::uint8_t event_fanout_rate(std::uint8_t kind) noexcept {
    return static_cast<std::uint8_t>(kind & 0x0F);
}

}  // namespace cedar
