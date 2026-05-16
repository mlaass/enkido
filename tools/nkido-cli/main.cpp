#include "asset_loader.hpp"
#include "bytecode_loader.hpp"
#include "bytecode_dump.hpp"
#include "audio_engine.hpp"
#include "program_loader.hpp"
#include "serve_mode.hpp"
#include "ui/ui_mode.hpp"
#include "akkado/akkado.hpp"
#include "cedar/vm/vm.hpp"
#include "cedar/opcodes/dsp_state.hpp"
#include "cedar/io/file_cache.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace {

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [mode] [options] [input]\n\n"
              << "Modes:\n"
              << "  play      Compile (if needed) and play audio (default)\n"
              << "  dump      Display bytecode in human-readable format\n"
              << "  compile   Compile source to bytecode file\n"
              << "  check     Syntax check only\n"
              << "  ui        Interactive editor mode\n"
              << "  render    Compile + render to WAV (offline)\n"
              << "  serve     Headless mode: read JSON commands from stdin\n\n"
              << "Input:\n"
              << "  <file.akkado>   Akkado source file\n"
              << "  <file.cedar>    Cedar bytecode file\n"
              << "  --source <code> Inline source string\n"
              << "  -               Read from stdin\n\n"
              << "Options:\n"
              << "  -r, --rate <hz>    Sample rate (default: 48000)\n"
              << "  -b, --buffer <n>   Buffer size (default: 128)\n"
              << "  --dump-bytecode    Show bytecode before playing\n"
              << "  --json             JSON output for errors/dump\n"
              << "  -v, --verbose      Show compilation stats\n"
              << "  -o, --output <f>   Output file (for compile/render mode)\n"
              << "  --seconds <f>      Render duration in seconds (default: 4)\n"
              << "  --bpm <f>          Override patch BPM in render mode (default: from patch, fallback 120)\n"
              << "  --trace-poly <f>   Write per-block poly voice state to JSONL\n"
              << "  --list-devices     List audio capture devices and exit\n"
              << "  --input-device <n> Capture device name for in() (default: system default)\n"
              << "  --list-midi-devices List MIDI input ports and exit\n"
              << "  --midi-in <name>   MIDI input port substring for bare midi() (default: first available)\n"
              << "  --bank <uri>       Sample-bank manifest URI (strudel.json). May repeat.\n"
              << "                     Schemes: file://, http(s)://, github:user/repo, bundled://...\n"
              << "                     Bare paths are treated as file://.\n"
              << "  --soundfont <uri>  SoundFont (SF2) URI. May repeat.\n"
              << "  --sample <uri>     Single-sample URI ('name=uri' or just URI). May repeat.\n"
              << "  --no-default-bank  Suppress the built-in default 808 kit.\n"
              << "  --no-default-soundfont  Suppress auto-loading of patch-declared SoundFonts (gm/gm_medium/gm_large).\n"
              << "                     (Override path with NKIDO_DEFAULT_KIT env var.)\n"
              << "  -h, --help         Show this help\n\n"
              << "Examples:\n"
              << "  " << program << " play song.akkado\n"
              << "  " << program << " --source \"sine(440) |> out(%,%)\" play\n"
              << "  cat song.akkado | " << program << " play -\n"
              << "  " << program << " dump song.cedar\n"
              << "  " << program << " compile -o out.cedar song.akkado\n"
              << "  " << program << " ui\n";
}

std::optional<nkido::Options> parse_args(int argc, char* argv[]) {
    nkido::Options opts;
    bool has_mode = false;
    bool has_input = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Modes
        if (arg == "play" && !has_mode) {
            opts.mode = nkido::Mode::Play;
            has_mode = true;
        } else if (arg == "dump" && !has_mode) {
            opts.mode = nkido::Mode::Dump;
            has_mode = true;
        } else if (arg == "compile" && !has_mode) {
            opts.mode = nkido::Mode::Compile;
            has_mode = true;
        } else if (arg == "check" && !has_mode) {
            opts.mode = nkido::Mode::Check;
            has_mode = true;
        } else if (arg == "ui" && !has_mode) {
            opts.mode = nkido::Mode::UI;
            has_mode = true;
        } else if (arg == "render" && !has_mode) {
            opts.mode = nkido::Mode::Render;
            has_mode = true;
        } else if (arg == "serve" && !has_mode) {
            opts.mode = nkido::Mode::Serve;
            has_mode = true;
        }
        // Options
        else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else if (arg == "-r" || arg == "--rate") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return std::nullopt;
            }
            opts.sample_rate = static_cast<std::uint32_t>(std::stoul(argv[i]));
        } else if (arg == "-b" || arg == "--buffer") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return std::nullopt;
            }
            opts.buffer_size = static_cast<std::uint32_t>(std::stoul(argv[i]));
        } else if (arg == "--source") {
            if (++i >= argc) {
                std::cerr << "error: --source requires a value\n";
                return std::nullopt;
            }
            opts.input = argv[i];
            opts.input_type = nkido::InputType::InlineSource;
            has_input = true;
        } else if (arg == "-o" || arg == "--output") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return std::nullopt;
            }
            opts.output_file = argv[i];
        } else if (arg == "--seconds") {
            if (++i >= argc) {
                std::cerr << "error: --seconds requires a value\n";
                return std::nullopt;
            }
            opts.render_seconds = std::stof(argv[i]);
        } else if (arg == "--bpm") {
            if (++i >= argc) {
                std::cerr << "error: --bpm requires a value\n";
                return std::nullopt;
            }
            opts.render_bpm = std::stof(argv[i]);
        } else if (arg == "--trace-poly") {
            if (++i >= argc) {
                std::cerr << "error: --trace-poly requires a value\n";
                return std::nullopt;
            }
            opts.trace_poly_file = argv[i];
        } else if (arg == "--list-devices") {
            opts.list_devices = true;
        } else if (arg == "--input-device") {
            if (++i >= argc) {
                std::cerr << "error: --input-device requires a value\n";
                return std::nullopt;
            }
            opts.input_device = argv[i];
        } else if (arg == "--list-midi-devices") {
            opts.list_midi_devices = true;
        } else if (arg == "--midi-in") {
            if (++i >= argc) {
                std::cerr << "error: --midi-in requires a value\n";
                return std::nullopt;
            }
            opts.midi_device = argv[i];
        } else if (arg == "--bank") {
            if (++i >= argc) {
                std::cerr << "error: --bank requires a URI\n";
                return std::nullopt;
            }
            opts.bank_uris.push_back(argv[i]);
        } else if (arg == "--soundfont") {
            if (++i >= argc) {
                std::cerr << "error: --soundfont requires a URI\n";
                return std::nullopt;
            }
            opts.soundfont_uris.push_back(argv[i]);
        } else if (arg == "--sample") {
            if (++i >= argc) {
                std::cerr << "error: --sample requires a URI (or 'name=uri')\n";
                return std::nullopt;
            }
            opts.sample_uris.push_back(argv[i]);
        } else if (arg == "--no-default-bank") {
            opts.no_default_bank = true;
        } else if (arg == "--no-default-soundfont") {
            opts.no_default_soundfont = true;
        } else if (arg == "--dump-bytecode") {
            opts.dump_bytecode = true;
        } else if (arg == "--json") {
            opts.json_output = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-") {
            opts.input = "-";
            opts.input_type = nkido::InputType::Stdin;
            has_input = true;
        } else if (arg[0] != '-' && !has_input) {
            opts.input = arg;
            opts.input_type = nkido::detect_input_type(arg);
            has_input = true;
        } else {
            std::cerr << "error: unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }

    // Validate (UI/Serve modes and --list-devices / --list-midi-devices don't need input)
    if (!has_input && opts.input_type != nkido::InputType::InlineSource &&
        opts.mode != nkido::Mode::UI && opts.mode != nkido::Mode::Serve &&
        !opts.list_devices && !opts.list_midi_devices) {
        std::cerr << "error: no input specified\n";
        print_usage(argv[0]);
        return std::nullopt;
    }

    // Compile mode requires output file
    if (opts.mode == nkido::Mode::Compile && !opts.output_file) {
        // Default output filename
        if (opts.input_type == nkido::InputType::SourceFile) {
            auto pos = opts.input.rfind('.');
            if (pos != std::string::npos) {
                opts.output_file = opts.input.substr(0, pos) + ".cedar";
            } else {
                opts.output_file = opts.input + ".cedar";
            }
        } else {
            opts.output_file = "out.cedar";
        }
    }

    return opts;
}

int handle_check_mode(const nkido::Options& opts) {
    // For check mode, we only need to compile and report errors
    std::string source;
    std::string filename;

    if (opts.input_type == nkido::InputType::InlineSource) {
        source = opts.input;
        filename = "<inline>";
    } else if (opts.input_type == nkido::InputType::Stdin) {
        std::string line;
        while (std::getline(std::cin, line)) {
            source += line + "\n";
        }
        filename = "<stdin>";
    } else if (opts.input_type == nkido::InputType::SourceFile) {
        std::ifstream file(opts.input);
        if (!file) {
            std::cerr << "error: cannot open file: " << opts.input << "\n";
            return EXIT_FAILURE;
        }
        source = std::string((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        filename = opts.input;
    } else {
        std::cerr << "error: check mode requires source input\n";
        return EXIT_FAILURE;
    }

    auto result = akkado::compile(source, filename);

    if (result.success) {
        if (opts.verbose) {
            std::cerr << "OK: " << result.bytecode.size() / 16 << " instructions\n";
        }
        return EXIT_SUCCESS;
    }

    // Output errors
    for (const auto& diag : result.diagnostics) {
        if (opts.json_output) {
            std::cerr << akkado::format_diagnostic_json(diag) << "\n";
        } else {
            std::cerr << akkado::format_diagnostic(diag, source) << "\n";
        }
    }

    return EXIT_FAILURE;
}

// ----------------------------------------------------------------------------
// Render mode: compile + apply state inits + run VM offline + write WAV
// ----------------------------------------------------------------------------

// Minimal WAV writer (16-bit PCM stereo)
bool write_wav_16(const std::string& path, const std::vector<float>& interleaved,
                  std::uint32_t sample_rate, std::uint16_t channels) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    auto write_u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto write_u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    auto write_str = [&](const char* s) { out.write(s, 4); };

    const std::uint32_t num_samples = static_cast<std::uint32_t>(interleaved.size());
    const std::uint32_t data_size = num_samples * 2;  // 16-bit = 2 bytes per sample
    const std::uint32_t fmt_size = 16;
    const std::uint32_t riff_size = 36 + data_size;
    const std::uint32_t byte_rate = sample_rate * channels * 2;
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);

    write_str("RIFF"); write_u32(riff_size); write_str("WAVE");
    write_str("fmt "); write_u32(fmt_size);
    write_u16(1);                   // PCM format
    write_u16(channels);
    write_u32(sample_rate);
    write_u32(byte_rate);
    write_u16(block_align);
    write_u16(16);                  // bits per sample
    write_str("data"); write_u32(data_size);

    for (float f : interleaved) {
        // Clamp and convert to int16
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        std::int16_t s = static_cast<std::int16_t>(f * 32767.0f);
        out.write(reinterpret_cast<const char*>(&s), 2);
    }

    return out.good();
}

int handle_render_mode(const nkido::Options& opts) {
    auto load = nkido::load_bytecode(opts);
    if (!load.success) {
        std::cerr << load.error_message << "\n";
        return EXIT_FAILURE;
    }
    if (!load.compile_result) {
        std::cerr << "error: render mode needs source input (.akkado / inline / stdin)\n";
        return EXIT_FAILURE;
    }

    auto vm = std::make_unique<cedar::VM>();
    vm->set_sample_rate(static_cast<float>(opts.sample_rate));

    std::vector<std::vector<cedar::Sequence>> seq_storage;
    if (!nkido::load_and_prepare_immediate(*vm, opts, load, seq_storage, std::cerr)) {
        return EXIT_FAILURE;
    }
    // CLI --bpm wins over the patch's `bpm = ...` when explicitly set.
    if (opts.render_bpm) {
        vm->set_bpm(*opts.render_bpm);
    }

    // Find PolyAllocState IDs (for tracing)
    std::vector<std::uint32_t> poly_state_ids;
    for (const auto& init : load.compile_result->state_inits) {
        if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            poly_state_ids.push_back(init.state_id);
        }
    }

    // PRD prd-midi-input §7.4: gather File-kind midi state_ids and build a
    // CC route table for offline dispatch. The render path doesn't go
    // through AudioEngine::apply_midi_route_plan, so do it inline here so
    // `midi({file:...}) |> ... ; midi_cc(...)` works in `nkido-cli render`.
    std::vector<std::uint32_t> file_midi_state_ids;
    for (const auto& req : load.compile_result->required_midi_sources) {
        if (req.kind == cedar::MidiSourceKind::File) {
            file_midi_state_ids.push_back(req.state_id);
        }
    }
    nkido::MidiCcRouteTable file_cc_table;
    file_cc_table.reserve(load.compile_result->required_midi_cc_routes.size());
    for (const auto& r : load.compile_result->required_midi_cc_routes) {
        file_cc_table.push_back({r.param_name, r.cc_num, r.channel_filter,
                                 r.scale, r.bias, r.slew_ms});
    }

    // Determine output WAV path
    std::string wav_path = opts.output_file.value_or("out.wav");

    // Optional poly trace
    std::ofstream trace;
    if (opts.trace_poly_file) {
        trace.open(*opts.trace_poly_file);
        if (!trace) {
            std::cerr << "error: cannot open trace file: " << *opts.trace_poly_file << "\n";
            return EXIT_FAILURE;
        }
    }

    // Render loop
    const std::uint32_t total_blocks = static_cast<std::uint32_t>(
        opts.render_seconds * static_cast<float>(opts.sample_rate) / cedar::BLOCK_SIZE);
    std::vector<float> interleaved;
    interleaved.reserve(total_blocks * cedar::BLOCK_SIZE * 2);

    std::array<float, cedar::BLOCK_SIZE> left{}, right{};

    for (std::uint32_t b = 0; b < total_blocks; ++b) {
        vm->process_block(left.data(), right.data());

        // PRD §7.4: drain file-CC events and dispatch through the route
        // table so render mode can exercise file-CC → param() bindings.
        if (!file_midi_state_ids.empty() && !file_cc_table.empty()) {
            for (std::uint32_t state_id : file_midi_state_ids) {
                vm->drain_pending_file_ccs(
                    state_id,
                    [&](std::uint8_t status, std::uint8_t d1,
                        std::uint8_t d2, std::uint8_t channel) {
                        nkido::dispatch_midi_cc(
                            *vm, file_cc_table, status, d1, d2,
                            static_cast<std::uint8_t>(channel + 1u));
                    });
            }
        } else if (!file_midi_state_ids.empty()) {
            // Drain without dispatch to keep the buffer counter clean.
            for (std::uint32_t state_id : file_midi_state_ids) {
                vm->drain_pending_file_ccs(
                    state_id,
                    [](std::uint8_t, std::uint8_t, std::uint8_t,
                       std::uint8_t) {});
            }
        }

        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            interleaved.push_back(left[i]);
            interleaved.push_back(right[i]);
        }

        // Write per-block voice state for each PolyAllocState
        if (trace.is_open()) {
            for (std::uint32_t state_id : poly_state_ids) {
                auto* poly = vm->states().get_if<cedar::PolyAllocState>(state_id);
                if (!poly || !poly->voices) continue;
                trace << "{\"block\":" << b
                      << ",\"state_id\":" << state_id
                      << ",\"max_voices\":" << static_cast<int>(poly->max_voices)
                      << ",\"voices\":[";
                bool first = true;
                for (std::uint16_t v = 0; v < poly->max_voices; ++v) {
                    const auto& voice = poly->voices[v];
                    if (!voice.active) continue;
                    if (!first) trace << ",";
                    first = false;
                    trace << "{\"i\":" << v
                          << ",\"freq\":" << voice.freq
                          << ",\"vel\":" << voice.vel
                          << ",\"gate\":" << voice.gate
                          << ",\"releasing\":" << (voice.releasing ? "true" : "false")
                          << ",\"age\":" << voice.age
                          << ",\"event\":" << voice.event_index
                          << ",\"cycle\":" << voice.cycle
                          << "}";
                }
                trace << "]}\n";
            }
        }
    }

    // Write WAV
    if (!write_wav_16(wav_path, interleaved, opts.sample_rate, 2)) {
        std::cerr << "error: failed to write WAV: " << wav_path << "\n";
        return EXIT_FAILURE;
    }

    if (opts.verbose) {
        std::cerr << "Rendered " << opts.render_seconds << "s "
                  << "(" << total_blocks << " blocks, "
                  << total_blocks * cedar::BLOCK_SIZE << " samples) to "
                  << wav_path << "\n";
        if (opts.trace_poly_file) {
            std::cerr << "Poly trace: " << *opts.trace_poly_file << "\n";
        }
    }

    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Parse arguments
    auto opts = parse_args(argc, argv);
    if (!opts) {
        return EXIT_FAILURE;
    }

    // Set up the URI resolver with native handlers (file:// http(s)://
    // github: bundled://). The cache is process-scoped under
    // $XDG_CACHE_HOME/nkido (or platform equivalent) and outlives all
    // resolver lookups so HTTP fetches benefit from disk caching across
    // runs.
    static cedar::FileCache uri_cache;
    nkido::register_native_handlers(uri_cache);

    // --list-devices stands on its own; no input required.
    if (opts->list_devices) {
        nkido::AudioEngine::list_capture_devices(std::cout);
        return EXIT_SUCCESS;
    }

    // Same for --list-midi-devices.
    if (opts->list_midi_devices) {
        nkido::AudioEngine::list_midi_devices(std::cout);
        return EXIT_SUCCESS;
    }

    // Handle check mode separately
    if (opts->mode == nkido::Mode::Check) {
        return handle_check_mode(*opts);
    }

    // Handle render mode (offline WAV)
    if (opts->mode == nkido::Mode::Render) {
        return handle_render_mode(*opts);
    }

    // Handle UI mode
    if (opts->mode == nkido::Mode::UI) {
        nkido::ui::UIMode ui;
        if (!ui.init(*opts)) {
            std::cerr << "error: failed to initialize UI\n";
            return EXIT_FAILURE;
        }
        return ui.run();
    }

    // Handle serve mode (headless JSON-over-stdio for editor integration)
    if (opts->mode == nkido::Mode::Serve) {
        return nkido::run_serve_mode(*opts);
    }

    // Load/compile bytecode
    auto result = nkido::load_bytecode(*opts);
    if (!result.success) {
        std::cerr << result.error_message << "\n";
        return EXIT_FAILURE;
    }

    // Show compilation stats if verbose
    if (opts->verbose && result.stats) {
        std::cerr << "Compiled " << result.stats->source_bytes << " bytes "
                  << "to " << result.stats->instruction_count << " instructions "
                  << "in " << result.stats->compile_time_ms << " ms\n";
    }

    // Handle dump mode or --dump-bytecode
    if (opts->mode == nkido::Mode::Dump || opts->dump_bytecode) {
        if (opts->json_output) {
            std::cout << nkido::format_program_json(result.instructions);
        } else {
            std::cout << nkido::format_program(result.instructions);
        }

        if (opts->mode == nkido::Mode::Dump) {
            return EXIT_SUCCESS;
        }
    }

    // Handle compile mode
    if (opts->mode == nkido::Mode::Compile) {
        if (!opts->output_file) {
            std::cerr << "error: no output file specified\n";
            return EXIT_FAILURE;
        }

        if (!nkido::write_bytecode_file(*opts->output_file, result.instructions)) {
            std::cerr << "error: failed to write output file: " << *opts->output_file << "\n";
            return EXIT_FAILURE;
        }

        if (opts->verbose) {
            std::cerr << "Wrote " << result.instructions.size() << " instructions to "
                      << *opts->output_file << "\n";
        }

        return EXIT_SUCCESS;
    }

    // Play mode - initialize audio engine
    nkido::AudioEngine engine;
    nkido::AudioEngine::Config audio_config{
        opts->sample_rate,
        opts->buffer_size,
        2  // stereo
    };

    if (!engine.init(audio_config)) {
        std::cerr << "error: failed to initialize audio\n";
        return EXIT_FAILURE;
    }

    // Open capture device if the program contains an INPUT instruction (i.e.
    // uses in()) or if --input-device was explicitly requested. Failure is
    // non-fatal — in() returns silence per the audio-input PRD contract.
    bool program_uses_input = false;
    for (const auto& inst : result.instructions) {
        if (inst.opcode == cedar::Opcode::INPUT) {
            program_uses_input = true;
            break;
        }
    }
    if (opts->input_device.has_value() || program_uses_input) {
        const char* dev = opts->input_device ? opts->input_device->c_str() : nullptr;
        engine.init_capture(dev);
    }

    // Record the preferred MIDI input for resolving bare midi() / DefaultDevice
    // entries. apply_midi_route_plan() below opens the actual port lazily
    // when the program contains midi() calls.
    engine.set_preferred_midi_device(
        opts->midi_device ? opts->midi_device->c_str() : nullptr);

    // Resolve assets, load program, and apply state inits in one pass.
    // seq_storage is a local of main(); engine.stop() (below) joins the audio
    // thread before this scope exits, so the sequence backing memory the VM
    // reads during playback stays alive for the entire run.
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    if (!nkido::load_and_prepare_immediate(engine.vm(), *opts, result, seq_storage, std::cerr)) {
        return EXIT_FAILURE;
    }

    // Open MIDI input ports referenced by midi() calls and route incoming
    // events into their MidiQueueState SPSC rings. Skipped when the input
    // is precompiled .cedar (no required_midi_sources metadata available).
    if (result.compile_result) {
        engine.apply_midi_route_plan(result.compile_result->required_midi_sources,
                                     result.compile_result->required_midi_cc_routes);
    }

    // Install signal handlers for graceful shutdown
    nkido::install_signal_handlers();

    // Start playback
    if (!engine.start()) {
        std::cerr << "error: failed to start audio playback\n";
        return EXIT_FAILURE;
    }

    std::cerr << "Playing... (Ctrl+C to stop)\n";
    if (opts->verbose) {
        std::cerr << "Sample rate: " << opts->sample_rate << " Hz\n";
        std::cerr << "Buffer size: " << opts->buffer_size << " samples\n";
        std::cerr << "Instructions: " << result.instructions.size() << "\n";
    }

    // Wait for shutdown signal
    engine.wait_for_shutdown();

    // Clean shutdown
    engine.stop();
    std::cerr << "\nStopped.\n";

    return EXIT_SUCCESS;
}
