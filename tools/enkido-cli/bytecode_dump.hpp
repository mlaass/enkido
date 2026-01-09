#pragma once

#include "cedar/vm/instruction.hpp"
#include <string>
#include <span>

namespace enkido {

// Get human-readable name for an opcode
const char* opcode_name(cedar::Opcode op);

// Format a single instruction
std::string format_instruction(const cedar::Instruction& inst, std::size_t index);

// Format entire program with statistics
std::string format_program(std::span<const cedar::Instruction> program);

// Format as JSON (for tooling integration)
std::string format_program_json(std::span<const cedar::Instruction> program);

}  // namespace enkido
