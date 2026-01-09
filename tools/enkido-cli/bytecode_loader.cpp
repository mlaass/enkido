#include "bytecode_loader.hpp"
#include "akkado/akkado.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstring>

namespace enkido {

InputType detect_input_type(const std::string& input) {
    if (input == "-") {
        return InputType::Stdin;
    }

    // Check file extension
    if (input.size() >= 7 && input.substr(input.size() - 7) == ".akkado") {
        return InputType::SourceFile;
    }
    if (input.size() >= 3 && input.substr(input.size() - 3) == ".ak") {
        return InputType::SourceFile;
    }
    if (input.size() >= 6 && input.substr(input.size() - 6) == ".cedar") {
        return InputType::BytecodeFile;
    }
    if (input.size() >= 3 && input.substr(input.size() - 3) == ".cb") {
        return InputType::BytecodeFile;
    }

    // Default: try as source file
    return InputType::SourceFile;
}

bool looks_like_bytecode(const std::vector<std::uint8_t>& data) {
    // Bytecode is 16-byte aligned instructions
    if (data.size() < 16 || data.size() % 16 != 0) {
        return false;
    }

    // Check if first byte looks like a valid opcode
    // Valid opcodes are 0-99 (approximately)
    if (data[0] > 100) {
        return false;
    }

    // Crude heuristic: source code usually starts with ASCII letters
    // or whitespace, bytecode starts with small numbers (opcodes)
    if (data[0] >= 32 && data[0] < 127) {
        // Looks like printable ASCII, probably source
        return false;
    }

    return true;
}

LoadResult compile_source(std::string_view source, std::string_view filename) {
    LoadResult result;

    auto start = std::chrono::high_resolution_clock::now();

    auto compile_result = akkado::compile(source, filename);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    if (!compile_result.success) {
        result.success = false;
        // Format error messages
        for (const auto& diag : compile_result.diagnostics) {
            if (!result.error_message.empty()) {
                result.error_message += "\n";
            }
            result.error_message += akkado::format_diagnostic(diag, source);
        }
        return result;
    }

    // Convert bytecode to instructions
    std::size_t num_instructions = compile_result.bytecode.size() / sizeof(cedar::Instruction);
    result.instructions.resize(num_instructions);
    std::memcpy(result.instructions.data(), compile_result.bytecode.data(),
                compile_result.bytecode.size());

    result.success = true;
    result.stats = LoadStats{
        source.size(),
        num_instructions,
        static_cast<float>(duration.count()) / 1000.0f
    };

    return result;
}

LoadResult compile_file(const std::string& path) {
    LoadResult result;

    // Read source file
    std::ifstream file(path);
    if (!file) {
        result.error_message = "error: cannot open file: " + path;
        return result;
    }

    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    return compile_source(source, path);
}

LoadResult read_bytecode_file(const std::string& path) {
    LoadResult result;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error_message = "error: cannot open file: " + path;
        return result;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size % sizeof(cedar::Instruction) != 0) {
        result.error_message = "error: invalid bytecode file (size not multiple of 16)";
        return result;
    }

    // Read instructions
    std::size_t num_instructions = static_cast<std::size_t>(size) / sizeof(cedar::Instruction);
    result.instructions.resize(num_instructions);
    file.read(reinterpret_cast<char*>(result.instructions.data()),
              static_cast<std::streamsize>(size));

    if (!file) {
        result.error_message = "error: failed to read bytecode file";
        return result;
    }

    result.success = true;
    result.stats = LoadStats{0, num_instructions, 0.0f};

    return result;
}

LoadResult read_from_stdin() {
    LoadResult result;

    // Read all stdin
    std::vector<std::uint8_t> data;
    char buffer[4096];
    while (std::cin.read(buffer, sizeof(buffer)) || std::cin.gcount() > 0) {
        data.insert(data.end(), buffer, buffer + std::cin.gcount());
    }

    if (data.empty()) {
        result.error_message = "error: no input from stdin";
        return result;
    }

    // Auto-detect format
    if (looks_like_bytecode(data)) {
        // Treat as bytecode
        std::size_t num_instructions = data.size() / sizeof(cedar::Instruction);
        result.instructions.resize(num_instructions);
        std::memcpy(result.instructions.data(), data.data(), data.size());
        result.success = true;
        result.stats = LoadStats{0, num_instructions, 0.0f};
    } else {
        // Treat as source code
        std::string source(data.begin(), data.end());
        return compile_source(source, "<stdin>");
    }

    return result;
}

LoadResult load_bytecode(const Options& opts) {
    switch (opts.input_type) {
        case InputType::Stdin:
            return read_from_stdin();

        case InputType::SourceFile:
            return compile_file(opts.input);

        case InputType::BytecodeFile:
            return read_bytecode_file(opts.input);

        case InputType::InlineSource:
            return compile_source(opts.input, "<inline>");

        default:
            return LoadResult{false, {}, "error: unknown input type", std::nullopt};
    }
}

bool write_bytecode_file(const std::string& path,
                         std::span<const cedar::Instruction> instructions) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    file.write(reinterpret_cast<const char*>(instructions.data()),
               static_cast<std::streamsize>(instructions.size() * sizeof(cedar::Instruction)));

    return file.good();
}

}  // namespace enkido
