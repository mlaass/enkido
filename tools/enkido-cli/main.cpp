#include "bytecode_loader.hpp"
#include "bytecode_dump.hpp"
#include "audio_engine.hpp"
#include "ui/ui_mode.hpp"
#include "akkado/akkado.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>

namespace {

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [mode] [options] [input]\n\n"
              << "Modes:\n"
              << "  play      Compile (if needed) and play audio (default)\n"
              << "  dump      Display bytecode in human-readable format\n"
              << "  compile   Compile source to bytecode file\n"
              << "  check     Syntax check only\n"
              << "  ui        Interactive editor mode\n\n"
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
              << "  -o, --output <f>   Output file (for compile mode)\n"
              << "  -h, --help         Show this help\n\n"
              << "Examples:\n"
              << "  " << program << " play song.akkado\n"
              << "  " << program << " --source \"sin(440) |> out(%,%)\" play\n"
              << "  cat song.akkado | " << program << " play -\n"
              << "  " << program << " dump song.cedar\n"
              << "  " << program << " compile -o out.cedar song.akkado\n"
              << "  " << program << " ui\n";
}

std::optional<enkido::Options> parse_args(int argc, char* argv[]) {
    enkido::Options opts;
    bool has_mode = false;
    bool has_input = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Modes
        if (arg == "play" && !has_mode) {
            opts.mode = enkido::Mode::Play;
            has_mode = true;
        } else if (arg == "dump" && !has_mode) {
            opts.mode = enkido::Mode::Dump;
            has_mode = true;
        } else if (arg == "compile" && !has_mode) {
            opts.mode = enkido::Mode::Compile;
            has_mode = true;
        } else if (arg == "check" && !has_mode) {
            opts.mode = enkido::Mode::Check;
            has_mode = true;
        } else if (arg == "ui" && !has_mode) {
            opts.mode = enkido::Mode::UI;
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
            opts.input_type = enkido::InputType::InlineSource;
            has_input = true;
        } else if (arg == "-o" || arg == "--output") {
            if (++i >= argc) {
                std::cerr << "error: " << arg << " requires a value\n";
                return std::nullopt;
            }
            opts.output_file = argv[i];
        } else if (arg == "--dump-bytecode") {
            opts.dump_bytecode = true;
        } else if (arg == "--json") {
            opts.json_output = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-") {
            opts.input = "-";
            opts.input_type = enkido::InputType::Stdin;
            has_input = true;
        } else if (arg[0] != '-' && !has_input) {
            opts.input = arg;
            opts.input_type = enkido::detect_input_type(arg);
            has_input = true;
        } else {
            std::cerr << "error: unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }

    // Validate (UI mode doesn't need input)
    if (!has_input && opts.input_type != enkido::InputType::InlineSource &&
        opts.mode != enkido::Mode::UI) {
        std::cerr << "error: no input specified\n";
        print_usage(argv[0]);
        return std::nullopt;
    }

    // Compile mode requires output file
    if (opts.mode == enkido::Mode::Compile && !opts.output_file) {
        // Default output filename
        if (opts.input_type == enkido::InputType::SourceFile) {
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

int handle_check_mode(const enkido::Options& opts) {
    // For check mode, we only need to compile and report errors
    std::string source;
    std::string filename;

    if (opts.input_type == enkido::InputType::InlineSource) {
        source = opts.input;
        filename = "<inline>";
    } else if (opts.input_type == enkido::InputType::Stdin) {
        std::string line;
        while (std::getline(std::cin, line)) {
            source += line + "\n";
        }
        filename = "<stdin>";
    } else if (opts.input_type == enkido::InputType::SourceFile) {
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

}  // namespace

int main(int argc, char* argv[]) {
    // Parse arguments
    auto opts = parse_args(argc, argv);
    if (!opts) {
        return EXIT_FAILURE;
    }

    // Handle check mode separately
    if (opts->mode == enkido::Mode::Check) {
        return handle_check_mode(*opts);
    }

    // Handle UI mode
    if (opts->mode == enkido::Mode::UI) {
        enkido::ui::UIMode ui;
        if (!ui.init(opts->sample_rate, opts->buffer_size)) {
            std::cerr << "error: failed to initialize UI\n";
            return EXIT_FAILURE;
        }
        return ui.run();
    }

    // Load/compile bytecode
    auto result = enkido::load_bytecode(*opts);
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
    if (opts->mode == enkido::Mode::Dump || opts->dump_bytecode) {
        if (opts->json_output) {
            std::cout << enkido::format_program_json(result.instructions);
        } else {
            std::cout << enkido::format_program(result.instructions);
        }

        if (opts->mode == enkido::Mode::Dump) {
            return EXIT_SUCCESS;
        }
    }

    // Handle compile mode
    if (opts->mode == enkido::Mode::Compile) {
        if (!opts->output_file) {
            std::cerr << "error: no output file specified\n";
            return EXIT_FAILURE;
        }

        if (!enkido::write_bytecode_file(*opts->output_file, result.instructions)) {
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
    enkido::AudioEngine engine;
    enkido::AudioEngine::Config audio_config{
        opts->sample_rate,
        opts->buffer_size,
        2  // stereo
    };

    if (!engine.init(audio_config)) {
        std::cerr << "error: failed to initialize audio\n";
        return EXIT_FAILURE;
    }

    // Load program into VM
    auto load_result = engine.vm().load_program_immediate(result.instructions);
    if (!load_result) {
        std::cerr << "error: failed to load program into VM\n";
        return EXIT_FAILURE;
    }

    // Install signal handlers for graceful shutdown
    enkido::install_signal_handlers();

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
