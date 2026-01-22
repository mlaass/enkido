// cedar/bindings/bindings.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include "cedar/cedar.hpp"
#include "cedar/vm/vm.hpp"

namespace py = pybind11;

// Helper to hash strings for state IDs from Python
std::uint32_t hash_str(const std::string& s) {
    return cedar::fnv1a_hash_runtime(s.c_str(), s.length());
}

PYBIND11_MODULE(cedar_core, m) {
    m.doc() = "Cedar Audio Engine bindings";

    // --- Constants ---
    m.attr("BLOCK_SIZE") = cedar::BLOCK_SIZE;
    m.attr("DEFAULT_SAMPLE_RATE") = cedar::DEFAULT_SAMPLE_RATE;

    m.def("hash", &hash_str, "Calculate FNV-1a hash for state IDs");

    // --- Opcodes ---
    py::enum_<cedar::Opcode>(m, "Opcode")
        .value("NOP", cedar::Opcode::NOP)
        .value("PUSH_CONST", cedar::Opcode::PUSH_CONST)
        .value("COPY", cedar::Opcode::COPY)
        // Arithmetic
        .value("ADD", cedar::Opcode::ADD)
        .value("SUB", cedar::Opcode::SUB)
        .value("MUL", cedar::Opcode::MUL)
        .value("DIV", cedar::Opcode::DIV)
        // Oscillators
        .value("OSC_SIN", cedar::Opcode::OSC_SIN)
        .value("OSC_TRI", cedar::Opcode::OSC_TRI)
        .value("OSC_SAW", cedar::Opcode::OSC_SAW)
        .value("OSC_SQR", cedar::Opcode::OSC_SQR)
        .value("OSC_RAMP", cedar::Opcode::OSC_RAMP)
        .value("OSC_PHASOR", cedar::Opcode::OSC_PHASOR)
        .value("OSC_SQR_MINBLEP", cedar::Opcode::OSC_SQR_MINBLEP)
        // PWM Oscillators
        .value("OSC_SQR_PWM", cedar::Opcode::OSC_SQR_PWM)
        .value("OSC_SAW_PWM", cedar::Opcode::OSC_SAW_PWM)
        .value("OSC_SQR_PWM_MINBLEP", cedar::Opcode::OSC_SQR_PWM_MINBLEP)
        // Oversampled Oscillators
        .value("OSC_SIN_2X", cedar::Opcode::OSC_SIN_2X)
        .value("OSC_SIN_4X", cedar::Opcode::OSC_SIN_4X)
        .value("OSC_SAW_2X", cedar::Opcode::OSC_SAW_2X)
        .value("OSC_SAW_4X", cedar::Opcode::OSC_SAW_4X)
        .value("OSC_SQR_2X", cedar::Opcode::OSC_SQR_2X)
        .value("OSC_SQR_4X", cedar::Opcode::OSC_SQR_4X)
        .value("OSC_TRI_2X", cedar::Opcode::OSC_TRI_2X)
        .value("OSC_TRI_4X", cedar::Opcode::OSC_TRI_4X)
        .value("OSC_SQR_PWM_4X", cedar::Opcode::OSC_SQR_PWM_4X)
        .value("OSC_SAW_PWM_4X", cedar::Opcode::OSC_SAW_PWM_4X)
        // Filters
        .value("FILTER_SVF_LP", cedar::Opcode::FILTER_SVF_LP)
        .value("FILTER_SVF_HP", cedar::Opcode::FILTER_SVF_HP)
        .value("FILTER_SVF_BP", cedar::Opcode::FILTER_SVF_BP)
        .value("FILTER_MOOG", cedar::Opcode::FILTER_MOOG)
        // Samplers
        .value("SAMPLE_PLAY", cedar::Opcode::SAMPLE_PLAY)
        .value("SAMPLE_PLAY_LOOP", cedar::Opcode::SAMPLE_PLAY_LOOP)
        // Utility
        .value("OUTPUT", cedar::Opcode::OUTPUT)
        .value("NOISE", cedar::Opcode::NOISE)
        .value("MTOF", cedar::Opcode::MTOF)
        .value("ENV_GET", cedar::Opcode::ENV_GET)
        // Envelopes
        .value("ENV_ADSR", cedar::Opcode::ENV_ADSR)
        .value("ENV_AR", cedar::Opcode::ENV_AR)
        .value("ENV_FOLLOWER", cedar::Opcode::ENV_FOLLOWER)
        // Sequencers & Timing
        .value("CLOCK", cedar::Opcode::CLOCK)
        .value("LFO", cedar::Opcode::LFO)
        .value("SEQ_STEP", cedar::Opcode::SEQ_STEP)
        .value("EUCLID", cedar::Opcode::EUCLID)
        .value("TRIGGER", cedar::Opcode::TRIGGER)
        .value("TIMELINE", cedar::Opcode::TIMELINE)
        .export_values();

    // --- Instruction ---
    py::class_<cedar::Instruction>(m, "Instruction")
        .def(py::init<>())
        .def_readwrite("opcode", &cedar::Instruction::opcode)
        .def_readwrite("rate", &cedar::Instruction::rate)
        .def_readwrite("out_buffer", &cedar::Instruction::out_buffer)
        .def_readwrite("state_id", &cedar::Instruction::state_id)
        // Factory methods
        .def_static("make_nullary", &cedar::Instruction::make_nullary,
            py::arg("op"), py::arg("out"), py::arg("state")=0)
        .def_static("make_unary", &cedar::Instruction::make_unary,
            py::arg("op"), py::arg("out"), py::arg("in0"), py::arg("state")=0)
        .def_static("make_binary", &cedar::Instruction::make_binary,
            py::arg("op"), py::arg("out"), py::arg("in0"), py::arg("in1"), py::arg("state")=0)
        .def_static("make_ternary", &cedar::Instruction::make_ternary,
            py::arg("op"), py::arg("out"), py::arg("in0"), py::arg("in1"), py::arg("in2"), py::arg("state")=0)
        .def_static("make_quaternary", &cedar::Instruction::make_quaternary,
            py::arg("op"), py::arg("out"), py::arg("in0"), py::arg("in1"), py::arg("in2"), py::arg("in3"), py::arg("state")=0);

    // --- VM ---
    py::class_<cedar::VM>(m, "VM")
        .def(py::init<>())
        .def("set_sample_rate", &cedar::VM::set_sample_rate)
        .def("set_bpm", &cedar::VM::set_bpm)
        .def("reset", &cedar::VM::reset)

        // Parameter binding
        .def("set_param", [](cedar::VM& vm, const char* name, float value) {
            return vm.set_param(name, value);
        })

        // Program loading (using immediate mode for testing)
        .def("load_program", [](cedar::VM& vm, const std::vector<cedar::Instruction>& prog) {
            // Need to convert vector to span for C++ API
            return vm.load_program_immediate(std::span<const cedar::Instruction>(prog.data(), prog.size()));
        })

        // Audio processing: returns (left, right) numpy arrays
        .def("process", [](cedar::VM& vm) {
            py::array_t<float> left(cedar::BLOCK_SIZE);
            py::array_t<float> right(cedar::BLOCK_SIZE);

            // Access raw pointers for C++ API
            vm.process_block(left.mutable_data(), right.mutable_data());

            return py::make_tuple(left, right);
        })

        // Buffer inspection (read register values)
        .def("get_buffer", [](cedar::VM& vm, uint16_t index) {
            if (index >= cedar::MAX_BUFFERS) throw std::out_of_range("Buffer index out of range");

            // Create a copy to return to Python
            py::array_t<float> result(cedar::BLOCK_SIZE);
            const float* src = vm.buffers().get(index);
            std::copy(src, src + cedar::BLOCK_SIZE, result.mutable_data());
            return result;
        })

        // Buffer injection (write register values for test signals)
        .def("set_buffer", [](cedar::VM& vm, uint16_t index, py::array_t<float> data) {
            if (index >= cedar::MAX_BUFFERS) throw std::out_of_range("Buffer index out of range");
            if (data.size() != cedar::BLOCK_SIZE) throw std::length_error("Data must be BLOCK_SIZE");

            float* dst = vm.buffers().get(index);
            // safe access
            auto r = data.unchecked<1>();
            for (size_t i = 0; i < cedar::BLOCK_SIZE; i++) {
                dst[i] = r(i);
            }
        })

        // Sample loading for sampler opcodes
        .def("load_sample", [](cedar::VM& vm, const std::string& name, py::array_t<float> data,
                               std::uint32_t channels, float sample_rate) {
            auto r = data.unchecked<1>();
            std::size_t num_samples = r.size();
            return vm.load_sample(name, r.data(0), num_samples, channels, sample_rate);
        }, py::arg("name"), py::arg("data"), py::arg("channels") = 1, py::arg("sample_rate") = 48000.0f);
}