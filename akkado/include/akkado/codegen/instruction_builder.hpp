#pragma once

// InstructionBuilder — PRD prd-codegen-sprawl-cleanup.md Phase 1.
//
// Fluent builder for cedar::Instruction. All five input slots default to
// 0xFFFF (unused), so call sites only name the slots they actually wire.
// Every emission routes through CodeGenerator::emit(), preserving the
// instructions_/source_locations_ parity invariant (correctness PRD F2),
// and buffer allocation routes through CodeGenerator::alloc_buffer(), the
// single E101 "Buffer pool exhausted" diagnostic path.

#include "akkado/codegen.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <initializer_list>

namespace akkado {
namespace codegen {

class InstructionBuilder {
public:
    explicit InstructionBuilder(cedar::Opcode op) {
        inst_.opcode = op;
        inst_.inputs[0] = 0xFFFF;
        inst_.inputs[1] = 0xFFFF;
        inst_.inputs[2] = 0xFFFF;
        inst_.inputs[3] = 0xFFFF;
        inst_.inputs[4] = 0xFFFF;
    }

    /// Set the input buffer at slot 0-4. Unset slots stay 0xFFFF.
    InstructionBuilder& input(int slot, std::uint16_t buf) {
        inst_.inputs[slot] = buf;
        return *this;
    }

    /// Set inputs from slot 0 upward; remaining slots stay 0xFFFF.
    InstructionBuilder& inputs(std::initializer_list<std::uint16_t> bufs) {
        int slot = 0;
        for (std::uint16_t b : bufs) inst_.inputs[slot++] = b;
        return *this;
    }

    /// Pre-set the output buffer (for callers that allocated it themselves,
    /// e.g. adjacent stereo pairs). Pair with emit(cg).
    InstructionBuilder& output(std::uint16_t buf) {
        inst_.out_buffer = buf;
        return *this;
    }

    InstructionBuilder& rate(std::uint8_t r) {
        inst_.rate = r;
        return *this;
    }

    InstructionBuilder& state_id(std::uint32_t id) {
        inst_.state_id = id;
        return *this;
    }

    InstructionBuilder& flags(std::uint16_t f) {
        inst_.flags = f;
        return *this;
    }

    /// PUSH_CONST payload: the float lives in state_id with inputs[4]
    /// forced to 0xFFFF (same encoding as codegen::encode_const_value).
    InstructionBuilder& const_value(float v) {
        std::memcpy(&inst_.state_id, &v, sizeof(float));
        inst_.inputs[4] = 0xFFFF;
        return *this;
    }

    /// Emit with the output set via output(). Use when the caller manages
    /// its own output-buffer allocation.
    void emit(CodeGenerator& cg) { cg.emit(inst_); }

    /// Allocate the output buffer via cg, emit, and return the buffer index.
    /// On pool exhaustion the E101 diagnostic is emitted at `loc` and
    /// BufferAllocator::BUFFER_UNUSED is returned without emitting.
    [[nodiscard]] std::uint16_t emit(CodeGenerator& cg, SourceLocation loc) {
        const std::uint16_t out = cg.alloc_buffer(loc);
        if (out == BufferAllocator::BUFFER_UNUSED) return out;
        inst_.out_buffer = out;
        cg.emit(inst_);
        return out;
    }

    /// The instruction as built so far — for sites that hand off to a
    /// non-standard emission path (e.g. insert-before, subprogram streams).
    [[nodiscard]] const cedar::Instruction& get() const { return inst_; }

private:
    cedar::Instruction inst_{};
};

} // namespace codegen
} // namespace akkado
