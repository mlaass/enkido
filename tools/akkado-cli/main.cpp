#include <iostream>
#include <fstream>
#include <cstdlib>
#include "akkado/akkado.hpp"

void print_usage(const char* program) {
    std::cout << "Akkado Compiler v" << akkado::Version::string() << "\n\n"
              << "Usage: " << program << " [options] <source-file>\n\n"
              << "Options:\n"
              << "  -h, --help           Show this help message\n"
              << "  -v, --version        Show version information\n"
              << "  -o, --output <file>  Output bytecode file (default: <input>.cedar)\n"
              << "  --json               Output diagnostics as JSON (for LSP/tooling)\n"
              << "  --check              Check syntax only, don't generate bytecode\n"
              << "  --samples            List required samples\n"
              << std::endl;
}

void print_version() {
    std::cout << "akkado " << akkado::Version::string() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::string input_file;
    std::string output_file;
    bool json_output = false;
    bool check_only = false;
    bool list_samples = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }

        if (arg == "-v" || arg == "--version") {
            print_version();
            return EXIT_SUCCESS;
        }

        if (arg == "--json") {
            json_output = true;
            continue;
        }

        if (arg == "--check") {
            check_only = true;
            continue;
        }

        if (arg == "--samples") {
            list_samples = true;
            continue;
        }

        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_file = argv[++i];
            continue;
        }

        // Assume it's the input file
        if (input_file.empty()) {
            input_file = arg;
        } else {
            std::cerr << "error: multiple input files not supported\n";
            return EXIT_FAILURE;
        }
    }

    if (input_file.empty()) {
        std::cerr << "error: no input file specified\n";
        return EXIT_FAILURE;
    }

    // Default output file
    if (output_file.empty() && !check_only) {
        output_file = input_file;
        if (auto pos = output_file.rfind('.'); pos != std::string::npos) {
            output_file = output_file.substr(0, pos);
        }
        output_file += ".cedar";
    }

    // Compile
    auto result = akkado::compile_file(input_file);

    // Output diagnostics
    std::ifstream source_file(input_file);
    std::string source_content;
    if (source_file) {
        source_content.assign(
            std::istreambuf_iterator<char>(source_file),
            std::istreambuf_iterator<char>()
        );
    }

    for (const auto& diag : result.diagnostics) {
        if (json_output) {
            std::cout << akkado::format_diagnostic_json(diag) << "\n";
        } else {
            std::cerr << akkado::format_diagnostic(diag, source_content);
        }
    }

    if (!result.success) {
        return EXIT_FAILURE;
    }

    // List required samples
    if (list_samples && !result.required_samples.empty()) {
        if (json_output) {
            std::cout << "{\"required_samples\":[";
            for (size_t i = 0; i < result.required_samples.size(); ++i) {
                if (i > 0) std::cout << ",";
                std::cout << "\"" << result.required_samples[i] << "\"";
            }
            std::cout << "]}\n";
        } else {
            std::cout << "Required samples:\n";
            for (const auto& name : result.required_samples) {
                std::cout << "  " << name << "\n";
            }
        }
    }

    // Write bytecode
    if (!check_only && !result.bytecode.empty()) {
        std::ofstream out(output_file, std::ios::binary);
        if (!out) {
            std::cerr << "error: could not write to " << output_file << "\n";
            return EXIT_FAILURE;
        }
        out.write(reinterpret_cast<const char*>(result.bytecode.data()),
                  static_cast<std::streamsize>(result.bytecode.size()));
        std::cout << "Wrote " << result.bytecode.size() << " bytes to " << output_file << "\n";
    }

    return EXIT_SUCCESS;
}
