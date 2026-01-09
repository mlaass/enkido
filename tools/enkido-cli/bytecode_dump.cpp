#include "bytecode_dump.hpp"
#include "cedar/dsp/constants.hpp"
#include <sstream>
#include <iomanip>
#include <cstring>

namespace enkido {

const char* opcode_name(cedar::Opcode op) {
    switch (op) {
        // Stack/Constants
        case cedar::Opcode::NOP:        return "NOP";
        case cedar::Opcode::PUSH_CONST: return "PUSH_CONST";
        case cedar::Opcode::COPY:       return "COPY";

        // Arithmetic
        case cedar::Opcode::ADD:        return "ADD";
        case cedar::Opcode::SUB:        return "SUB";
        case cedar::Opcode::MUL:        return "MUL";
        case cedar::Opcode::DIV:        return "DIV";
        case cedar::Opcode::POW:        return "POW";
        case cedar::Opcode::NEG:        return "NEG";

        // Oscillators
        case cedar::Opcode::OSC_SIN:    return "OSC_SIN";
        case cedar::Opcode::OSC_TRI:    return "OSC_TRI";
        case cedar::Opcode::OSC_SAW:    return "OSC_SAW";
        case cedar::Opcode::OSC_SQR:    return "OSC_SQR";
        case cedar::Opcode::OSC_RAMP:   return "OSC_RAMP";
        case cedar::Opcode::OSC_PHASOR: return "OSC_PHASOR";

        // Filters (SVF only)
        case cedar::Opcode::FILTER_SVF_LP: return "FILTER_SVF_LP";
        case cedar::Opcode::FILTER_SVF_HP: return "FILTER_SVF_HP";
        case cedar::Opcode::FILTER_SVF_BP: return "FILTER_SVF_BP";

        // Math
        case cedar::Opcode::ABS:        return "ABS";
        case cedar::Opcode::SQRT:       return "SQRT";
        case cedar::Opcode::LOG:        return "LOG";
        case cedar::Opcode::EXP:        return "EXP";
        case cedar::Opcode::MIN:        return "MIN";
        case cedar::Opcode::MAX:        return "MAX";
        case cedar::Opcode::CLAMP:      return "CLAMP";
        case cedar::Opcode::WRAP:       return "WRAP";
        case cedar::Opcode::FLOOR:      return "FLOOR";
        case cedar::Opcode::CEIL:       return "CEIL";

        // Utility
        case cedar::Opcode::OUTPUT:     return "OUTPUT";
        case cedar::Opcode::NOISE:      return "NOISE";
        case cedar::Opcode::MTOF:       return "MTOF";
        case cedar::Opcode::DC:         return "DC";
        case cedar::Opcode::SLEW:       return "SLEW";
        case cedar::Opcode::SAH:        return "SAH";
        case cedar::Opcode::ENV_GET:    return "ENV_GET";

        // Envelopes
        case cedar::Opcode::ENV_ADSR:   return "ENV_ADSR";
        case cedar::Opcode::ENV_AR:     return "ENV_AR";

        // Delays
        case cedar::Opcode::DELAY:      return "DELAY";

        // Sequencing & Timing
        case cedar::Opcode::CLOCK:      return "CLOCK";
        case cedar::Opcode::LFO:        return "LFO";
        case cedar::Opcode::SEQ_STEP:   return "SEQ_STEP";
        case cedar::Opcode::EUCLID:     return "EUCLID";
        case cedar::Opcode::TRIGGER:    return "TRIGGER";
        case cedar::Opcode::TIMELINE:   return "TIMELINE";

        case cedar::Opcode::INVALID:    return "INVALID";
        default:                        return "UNKNOWN";
    }
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
            oss << " shape=" << (inst.reserved & 0xFF);
            break;

        case cedar::Opcode::CLOCK:
            oss << " mode=" << static_cast<int>(inst.rate);
            break;

        default:
            // Generic input display
            for (int i = 0; i < 3; ++i) {
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
            << inst.inputs[1] << ", " << inst.inputs[2] << "],\n";
        oss << "      \"reserved\": " << inst.reserved << ",\n";
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
