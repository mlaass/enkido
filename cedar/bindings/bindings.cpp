// cedar/bindings/bindings.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include "cedar/cedar.hpp"
#include "cedar/vm/vm.hpp"
#ifndef CEDAR_NO_FFT
#include "cedar/wavetable/preprocessor.hpp"
#endif

#include <cstdio>
#include <vector>

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
        // Stack/Constants (0-9)
        .value("NOP", cedar::Opcode::NOP)
        .value("PUSH_CONST", cedar::Opcode::PUSH_CONST)
        .value("COPY", cedar::Opcode::COPY)
        // Arithmetic (10-19)
        .value("ADD", cedar::Opcode::ADD)
        .value("SUB", cedar::Opcode::SUB)
        .value("MUL", cedar::Opcode::MUL)
        .value("DIV", cedar::Opcode::DIV)
        .value("POW", cedar::Opcode::POW)
        .value("NEG", cedar::Opcode::NEG)
        // Oscillators (20-29)
        .value("OSC_SIN", cedar::Opcode::OSC_SIN)
        .value("OSC_TRI", cedar::Opcode::OSC_TRI)
        .value("OSC_SAW", cedar::Opcode::OSC_SAW)
        .value("OSC_SQR", cedar::Opcode::OSC_SQR)
        .value("OSC_RAMP", cedar::Opcode::OSC_RAMP)
        .value("OSC_PHASOR", cedar::Opcode::OSC_PHASOR)
        .value("OSC_SQR_MINBLEP", cedar::Opcode::OSC_SQR_MINBLEP)
        .value("OSC_SQR_PWM", cedar::Opcode::OSC_SQR_PWM)
        .value("OSC_SAW_PWM", cedar::Opcode::OSC_SAW_PWM)
        .value("OSC_SQR_PWM_MINBLEP", cedar::Opcode::OSC_SQR_PWM_MINBLEP)
        // Filters (30-39)
        .value("FILTER_SVF_LP", cedar::Opcode::FILTER_SVF_LP)
        .value("FILTER_SVF_HP", cedar::Opcode::FILTER_SVF_HP)
        .value("FILTER_SVF_BP", cedar::Opcode::FILTER_SVF_BP)
        .value("FILTER_MOOG", cedar::Opcode::FILTER_MOOG)
        .value("FILTER_DIODE", cedar::Opcode::FILTER_DIODE)
        .value("FILTER_FORMANT", cedar::Opcode::FILTER_FORMANT)
        .value("FILTER_SALLENKEY", cedar::Opcode::FILTER_SALLENKEY)
        // Math (40-49)
        .value("ABS", cedar::Opcode::ABS)
        .value("SQRT", cedar::Opcode::SQRT)
        .value("LOG", cedar::Opcode::LOG)
        .value("EXP", cedar::Opcode::EXP)
        .value("MIN", cedar::Opcode::MIN)
        .value("MAX", cedar::Opcode::MAX)
        .value("CLAMP", cedar::Opcode::CLAMP)
        .value("WRAP", cedar::Opcode::WRAP)
        .value("FLOOR", cedar::Opcode::FLOOR)
        .value("CEIL", cedar::Opcode::CEIL)
        // Utility (50-59)
        .value("OUTPUT", cedar::Opcode::OUTPUT)
        .value("NOISE", cedar::Opcode::NOISE)
        .value("MTOF", cedar::Opcode::MTOF)
        .value("DC", cedar::Opcode::DC)
        .value("SLEW", cedar::Opcode::SLEW)
        .value("EDGE_OP", cedar::Opcode::EDGE_OP)
        // Backwards-compat alias for the SAH→EDGE_OP rename: same numeric value (55).
        // Python tests can still write `cedar.Opcode.SAH` and get rate=0 SAH behavior.
        .value("SAH", cedar::Opcode::EDGE_OP)
        .value("ENV_GET", cedar::Opcode::ENV_GET)
        .value("STATE_OP", cedar::Opcode::STATE_OP)
        .value("INTERP_TIME", cedar::Opcode::INTERP_TIME)
        // Envelopes (60-62)
        .value("ENV_ADSR", cedar::Opcode::ENV_ADSR)
        .value("ENV_AR", cedar::Opcode::ENV_AR)
        .value("ENV_FOLLOWER", cedar::Opcode::ENV_FOLLOWER)
        // Samplers (63-64)
        .value("SAMPLE_PLAY", cedar::Opcode::SAMPLE_PLAY)
        .value("SAMPLE_PLAY_LOOP", cedar::Opcode::SAMPLE_PLAY_LOOP)
        // SoundFont (65)
        .value("SOUNDFONT_VOICE", cedar::Opcode::SOUNDFONT_VOICE)
        // Delays & Reverbs (70-79)
        .value("DELAY", cedar::Opcode::DELAY)
        .value("REVERB_FREEVERB", cedar::Opcode::REVERB_FREEVERB)
        .value("REVERB_DATTORRO", cedar::Opcode::REVERB_DATTORRO)
        .value("REVERB_FDN", cedar::Opcode::REVERB_FDN)
        .value("DELAY_TAP", cedar::Opcode::DELAY_TAP)
        .value("DELAY_WRITE", cedar::Opcode::DELAY_WRITE)
        // Effects - Modulation (80-83)
        .value("EFFECT_CHORUS", cedar::Opcode::EFFECT_CHORUS)
        .value("EFFECT_FLANGER", cedar::Opcode::EFFECT_FLANGER)
        .value("EFFECT_PHASER", cedar::Opcode::EFFECT_PHASER)
        .value("EFFECT_COMB", cedar::Opcode::EFFECT_COMB)
        // Effects - Distortion (84-89, 96-99)
        .value("DISTORT_TANH", cedar::Opcode::DISTORT_TANH)
        .value("DISTORT_SOFT", cedar::Opcode::DISTORT_SOFT)
        .value("DISTORT_BITCRUSH", cedar::Opcode::DISTORT_BITCRUSH)
        .value("DISTORT_FOLD", cedar::Opcode::DISTORT_FOLD)
        .value("DISTORT_TUBE", cedar::Opcode::DISTORT_TUBE)
        .value("DISTORT_SMOOTH", cedar::Opcode::DISTORT_SMOOTH)
        .value("DISTORT_TAPE", cedar::Opcode::DISTORT_TAPE)
        .value("DISTORT_XFMR", cedar::Opcode::DISTORT_XFMR)
        .value("DISTORT_EXCITE", cedar::Opcode::DISTORT_EXCITE)
        // Sequencers & Timing (90-95)
        .value("CLOCK", cedar::Opcode::CLOCK)
        .value("LFO", cedar::Opcode::LFO)
        .value("EUCLID", cedar::Opcode::EUCLID)
        .value("TRIGGER", cedar::Opcode::TRIGGER)
        .value("TIMELINE", cedar::Opcode::TIMELINE)
        // Dynamics (100-109)
        .value("DYNAMICS_COMP", cedar::Opcode::DYNAMICS_COMP)
        .value("DYNAMICS_LIMITER", cedar::Opcode::DYNAMICS_LIMITER)
        .value("DYNAMICS_GATE", cedar::Opcode::DYNAMICS_GATE)
        // Oversampled Oscillators (110-119)
        .value("OSC_SIN_4X", cedar::Opcode::OSC_SIN_4X)
        .value("OSC_SAW_4X", cedar::Opcode::OSC_SAW_4X)
        .value("OSC_SQR_4X", cedar::Opcode::OSC_SQR_4X)
        .value("OSC_TRI_4X", cedar::Opcode::OSC_TRI_4X)
        .value("OSC_SQR_PWM_4X", cedar::Opcode::OSC_SQR_PWM_4X)
        .value("OSC_SAW_PWM_4X", cedar::Opcode::OSC_SAW_PWM_4X)
        // Wavetable Oscillators (200-209)
        .value("OSC_WAVETABLE", cedar::Opcode::OSC_WAVETABLE)
        // Polyphony (150-151)
        .value("POLY_BEGIN", cedar::Opcode::POLY_BEGIN)
        .value("POLY_END", cedar::Opcode::POLY_END)
        // Stereo Operations (170-179)
        .value("PAN", cedar::Opcode::PAN)
        .value("WIDTH", cedar::Opcode::WIDTH)
        .value("MS_ENCODE", cedar::Opcode::MS_ENCODE)
        .value("MS_DECODE", cedar::Opcode::MS_DECODE)
        .value("DELAY_PINGPONG", cedar::Opcode::DELAY_PINGPONG)
        .value("MONO_DOWNMIX", cedar::Opcode::MONO_DOWNMIX)
        .value("PAN_STEREO", cedar::Opcode::PAN_STEREO)
        // Signal mux + arrays (140, 160-166)
        .value("SELECT", cedar::Opcode::SELECT)
        .value("ARRAY_PACK", cedar::Opcode::ARRAY_PACK)
        .value("ARRAY_INDEX", cedar::Opcode::ARRAY_INDEX)
        .export_values();

    // Instruction flag constants (bit values)
    m.attr("STEREO_INPUT_FLAG") = cedar::InstructionFlag::STEREO_INPUT;
    m.attr("STEREO_OUTPUT_FLAG") = cedar::InstructionFlag::STEREO_OUTPUT;
    m.attr("EXT_PARAMS_STATE_XOR") = cedar::EXT_PARAMS_STATE_XOR;
    m.def("ext_params_state_id", &cedar::ext_params_state_id,
          py::arg("state_id"),
          "Sibling state_id for ExtendedParams<N> storage (XOR'd to avoid "
          "collision with the opcode's primary DSP state).");

    // --- Instruction ---
    py::class_<cedar::Instruction>(m, "Instruction")
        .def(py::init<>())
        .def_readwrite("opcode", &cedar::Instruction::opcode)
        .def_readwrite("rate", &cedar::Instruction::rate)
        .def_readwrite("out_buffer", &cedar::Instruction::out_buffer)
        .def_readwrite("flags", &cedar::Instruction::flags)
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
            py::arg("op"), py::arg("out"), py::arg("in0"), py::arg("in1"), py::arg("in2"), py::arg("in3"), py::arg("state")=0)
        .def_static("make_quinary", &cedar::Instruction::make_quinary,
            py::arg("op"), py::arg("out"), py::arg("in0"), py::arg("in1"), py::arg("in2"), py::arg("in3"), py::arg("in4"), py::arg("state")=0);

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

        // Polyphony state initialization
        .def("init_poly_state", &cedar::VM::init_poly_state,
            py::arg("state_id"), py::arg("seq_state_id"),
            py::arg("max_voices"), py::arg("mode"), py::arg("steal_strategy"))

        // Extended-params initialization (PRD prd-extended-params §5).
        // `constants` and `buffer_indices` must be equal-length arrays of
        // `count` entries. A buffer_index of 0xFFFF means "use the constant".
        .def("init_extended_params",
            [](cedar::VM& vm, std::uint32_t state_id,
               py::array_t<float> constants,
               py::array_t<std::uint16_t> buffer_indices,
               std::uint8_t count) {
                auto c = constants.unchecked<1>();
                auto b = buffer_indices.unchecked<1>();
                if (static_cast<std::size_t>(c.size()) < count ||
                    static_cast<std::size_t>(b.size()) < count) {
                    throw std::length_error(
                        "init_extended_params: arrays must hold at least `count` entries");
                }
                vm.init_extended_params(state_id, c.data(0), b.data(0), count);
            },
            py::arg("state_id"), py::arg("constants"),
            py::arg("buffer_indices"), py::arg("count"))

        // Sample loading for sampler opcodes
        .def("load_sample", [](cedar::VM& vm, const std::string& name, py::array_t<float> data,
                               std::uint32_t channels, float sample_rate) {
            auto r = data.unchecked<1>();
            std::size_t num_samples = r.size();
            return vm.load_sample(name, r.data(0), num_samples, channels, sample_rate);
        }, py::arg("name"), py::arg("data"), py::arg("channels") = 1, py::arg("sample_rate") = 48000.0f)

#ifndef CEDAR_NO_SOUNDFONT
        // Load a SoundFont (SF2/SF3) from a file on disk. Reads the file into
        // memory and registers it with the VM's SoundFontRegistry. Returns the
        // assigned SoundFont ID (>= 0). Same-name re-loads dedup to the same
        // ID. Raises RuntimeError if the file cannot be read; returns -1 if
        // the data is rejected as invalid SF2.
        .def("soundfont_load_from_file", [](cedar::VM& vm, const std::string& name,
                                            const std::string& path) {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) {
                throw std::runtime_error("Failed to open SoundFont file: " + path);
            }
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                std::fclose(f);
                throw std::runtime_error("SoundFont file is empty: " + path);
            }
            std::vector<unsigned char> buf(static_cast<std::size_t>(sz));
            std::size_t read = std::fread(buf.data(), 1, buf.size(), f);
            std::fclose(f);
            if (read != buf.size()) {
                throw std::runtime_error("Short read on SoundFont file: " + path);
            }
            return vm.soundfont_registry().load_from_memory(
                cedar::MemoryView(buf.data(), buf.size()),
                name, vm.sample_bank());
        }, py::arg("name"), py::arg("path"))

        // Load a SoundFont from raw bytes already in memory. Useful for tests
        // that want to drive the dedup / invalid-data paths without touching
        // the filesystem.
        .def("soundfont_load_from_memory", [](cedar::VM& vm, const std::string& name,
                                              py::bytes data) {
            std::string raw = data;  // implicit conversion to std::string
            return vm.soundfont_registry().load_from_memory(
                cedar::MemoryView(reinterpret_cast<const std::uint8_t*>(raw.data()),
                                  raw.size()),
                name, vm.sample_bank());
        }, py::arg("name"), py::arg("data"))

        // Number of currently registered SoundFonts.
        .def("soundfont_count", [](cedar::VM& vm) {
            return vm.soundfont_registry().size();
        })

        // JSON list of presets in a loaded SoundFont (matches the web UI format).
        .def("soundfont_presets_json", [](cedar::VM& vm, int sf_id) {
            return vm.soundfont_registry().get_presets_json(sf_id);
        }, py::arg("sf_id"))
#endif

#ifndef CEDAR_NO_FFT
        // Load a wavetable bank from a WAV file (native only). Returns the
        // assigned bank ID (>= 0). On failure raises RuntimeError.
        .def("wavetable_load_wav", [](cedar::VM& vm, const std::string& name,
                                       const std::string& path) {
            std::string err;
            const int id = vm.wavetable_registry().load_from_file(name, path, &err);
            if (id < 0) throw std::runtime_error(err);
            return id;
        }, py::arg("name"), py::arg("path"))

        // Register a synthetic wavetable bank from a 2-D numpy array shaped
        // (num_frames, 2048). Each row becomes one source frame. Returns
        // the assigned bank ID.
        .def("wavetable_register_synthetic", [](cedar::VM& vm,
                                                  const std::string& name,
                                                  py::array_t<float, py::array::c_style |
                                                                      py::array::forcecast> frames) {
            const auto buf = frames.request();
            if (buf.ndim != 2) {
                throw std::invalid_argument("frames must be 2-D (num_frames, 2048)");
            }
            const std::size_t num_frames  = static_cast<std::size_t>(buf.shape[0]);
            const std::size_t frame_size  = static_cast<std::size_t>(buf.shape[1]);
            if (frame_size != static_cast<std::size_t>(cedar::WAVETABLE_SIZE)) {
                throw std::invalid_argument(
                    "frame width must equal WAVETABLE_SIZE (2048), got "
                    + std::to_string(frame_size));
            }
            const std::size_t total = num_frames * frame_size;
            const float* data = static_cast<const float*>(buf.ptr);
            std::string err;
            auto bank = cedar::build_bank_from_samples(name, data, total, &err);
            if (!bank) throw std::runtime_error(err);
            return vm.wavetable_registry().set_named(name, std::move(bank));
        }, py::arg("name"), py::arg("frames"))

        // Drop all registered wavetable banks.
        .def("wavetable_clear", [](cedar::VM& vm) {
            vm.wavetable_registry().clear();
        })

        // True if a bank with this name is registered.
        .def("wavetable_has", [](cedar::VM& vm, const std::string& name) {
            return vm.wavetable_registry().has(name);
        }, py::arg("name"))

        // Look up the bank ID for a name; returns -1 if not registered.
        .def("wavetable_find_id", [](cedar::VM& vm, const std::string& name) {
            return vm.wavetable_registry().find_id(name);
        }, py::arg("name"))

        // Number of registered wavetable banks.
        .def("wavetable_count", [](cedar::VM& vm) {
            return vm.wavetable_registry().size();
        })

        // Expose the maximum bank count constant for tests / UI.
        .def_readonly_static("MAX_WAVETABLE_BANKS",
                             new std::size_t(cedar::MAX_WAVETABLE_BANKS))
#endif
        ;
}