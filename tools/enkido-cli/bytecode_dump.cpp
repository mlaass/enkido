#include "bytecode_dump.hpp"
#include "cedar/dsp/constants.hpp"
#include <cedar/generated/opcode_metadata.hpp>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace enkido {

const char* opcode_name(cedar::Opcode op) {
    return cedar::opcode_to_string(op);
}

std::string format_instruction(const cedar::Instruction& inst, std::size_t index) {
    std::ostringstream oss;

    // Index
    oss << std::setw(4) << std::setfill('0') << index << ": ";

    // Opcode name
    oss << std::left << std::setw(14) << std::setfill(' ') << opcode_name(inst.opcode);

    // Output buffer
    oss << "buf[" << std::setw(3) << inst.out_buffer << "]";

    // Operation details based on opcode
    switch (inst.opcode) {
        case cedar::Opcode::PUSH_CONST:
        case cedar::Opcode::DC: {
            // Constant stored in state_id field
            float value;
            std::memcpy(&value, &inst.state_id, sizeof(float));
            oss << " = " << std::fixed << std::setprecision(3) << value;
            break;
        }

        case cedar::Opcode::COPY:
        case cedar::Opcode::NEG:
        case cedar::Opcode::ABS:
        case cedar::Opcode::SQRT:
        case cedar::Opcode::LOG:
        case cedar::Opcode::EXP:
        case cedar::Opcode::FLOOR:
        case cedar::Opcode::CEIL:
        case cedar::Opcode::MTOF:
            if (inst.inputs[0] != cedar::BUFFER_UNUSED) {
                oss << " <- buf[" << inst.inputs[0] << "]";
            }
            break;

        case cedar::Opcode::ADD:
        case cedar::Opcode::SUB:
        case cedar::Opcode::MUL:
        case cedar::Opcode::DIV:
        case cedar::Opcode::POW:
        case cedar::Opcode::MIN:
        case cedar::Opcode::MAX:
            if (inst.inputs[0] != cedar::BUFFER_UNUSED && inst.inputs[1] != cedar::BUFFER_UNUSED) {
                oss << " <- buf[" << inst.inputs[0] << "], buf[" << inst.inputs[1] << "]";
            }
            break;

        case cedar::Opcode::CLAMP:
        case cedar::Opcode::WRAP:
            if (inst.inputs[0] != cedar::BUFFER_UNUSED) {
                oss << " <- buf[" << inst.inputs[0] << "]";
                if (inst.inputs[1] != cedar::BUFFER_UNUSED) {
                    oss << ", buf[" << inst.inputs[1] << "]";
                }
                if (inst.inputs[2] != cedar::BUFFER_UNUSED) {
                    oss << ", buf[" << inst.inputs[2] << "]";
                }
            }
            break;

        case cedar::Opcode::OSC_SIN:
        case cedar::Opcode::OSC_TRI:
        case cedar::Opcode::OSC_SAW:
        case cedar::Opcode::OSC_SQR:
        case cedar::Opcode::OSC_RAMP:
        case cedar::Opcode::OSC_PHASOR:
            if (inst.inputs[0] != cedar::BUFFER_UNUSED) {
                oss << " freq=buf[" << inst.inputs[0] << "]";
            }
            break;

        case cedar::Opcode::FILTER_SVF_LP:
        case cedar::Opcode::FILTER_SVF_HP:
        case cedar::Opcode::FILTER_SVF_BP:
            if (inst.inputs[0] != cedar::BUFFER_UNUSED) {
                oss << " in=buf[" << inst.inputs[0] << "]";
            }
            if (inst.inputs[1] != cedar::BUFFER_UNUSED) {
                oss << " freq=buf[" << inst.inputs[1] << "]";
            }
            if (inst.inputs[2] != cedar::BUFFER_UNUSED) {
                oss << " q=buf[" << inst.inputs[2] << "]";
            }
            break;

        case cedar::Opcode::OUTPUT:
            if (inst.inputs[0] != cedar::BUFFER_UNUSED) {
                oss << " <- buf[" << inst.inputs[0] << "]";
            }
            break;

        case cedar::Opcode::NOISE:
            oss << " (white noise)";
            break;

        case cedar::Opcode::LFO:
            if (inst.inputs[0] != cedar::BUFFER_UNUSED) {
                oss << " rate=buf[" << inst.inputs[0] << "]";
            }
            oss << " shape=" << static_cast<int>(inst.rate);
            break;

        case cedar::Opcode::CLOCK:
            oss << " mode=" << static_cast<int>(inst.rate);
            break;

        default:
            // Generic input display
            for (int i = 0; i < 4; ++i) {
                if (inst.inputs[i] != cedar::BUFFER_UNUSED) {
                    oss << " in" << i << "=buf[" << inst.inputs[i] << "]";
                }
            }
            break;
    }

    // State ID if present
    if (inst.state_id != 0 &&
        inst.opcode != cedar::Opcode::PUSH_CONST &&
        inst.opcode != cedar::Opcode::DC) {
        oss << "  state: 0x" << std::hex << std::setw(8) << std::setfill('0') << inst.state_id;
    }

    return oss.str();
}

std::string format_program(std::span<const cedar::Instruction> program) {
    std::ostringstream oss;

    oss << "Cedar Bytecode - " << program.size() << " instructions\n";
    oss << std::string(60, '=') << "\n";

    for (std::size_t i = 0; i < program.size(); ++i) {
        oss << format_instruction(program[i], i) << "\n";
    }

    oss << std::string(60, '=') << "\n";

    return oss.str();
}

std::string format_program_json(std::span<const cedar::Instruction> program) {
    std::ostringstream oss;

    oss << "{\n";
    oss << "  \"instruction_count\": " << program.size() << ",\n";
    oss << "  \"instructions\": [\n";

    for (std::size_t i = 0; i < program.size(); ++i) {
        const auto& inst = program[i];
        oss << "    {\n";
        oss << "      \"index\": " << i << ",\n";
        oss << "      \"opcode\": \"" << opcode_name(inst.opcode) << "\",\n";
        oss << "      \"opcode_value\": " << static_cast<int>(inst.opcode) << ",\n";
        oss << "      \"rate\": " << static_cast<int>(inst.rate) << ",\n";
        oss << "      \"out_buffer\": " << inst.out_buffer << ",\n";
        oss << "      \"inputs\": [" << inst.inputs[0] << ", "
            << inst.inputs[1] << ", " << inst.inputs[2] << ", " << inst.inputs[3] << "],\n";
        oss << "      \"state_id\": " << inst.state_id << "\n";
        oss << "    }";
        if (i < program.size() - 1) oss << ",";
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}\n";

    return oss.str();
}

}  // namespace enkido
