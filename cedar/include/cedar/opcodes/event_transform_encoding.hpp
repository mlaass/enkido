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

}  // namespace cedar
