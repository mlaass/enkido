#include "akkado/akkado.hpp"
#include <fstream>
#include <sstream>

namespace akkado {

CompileResult compile(std::string_view source, std::string_view filename) {
    CompileResult result;

    // TODO: Implement lexer, parser, semantic analysis, and codegen
    // For now, return a placeholder error

    if (source.empty()) {
        result.diagnostics.push_back(Diagnostic{
            .severity = Severity::Error,
            .code = "E001",
            .message = "Empty source file",
            .filename = std::string(filename),
            .location = {.line = 1, .column = 1, .offset = 0, .length = 0}
        });
        result.success = false;
        return result;
    }

    // Placeholder: compilation not yet implemented
    result.diagnostics.push_back(Diagnostic{
        .severity = Severity::Info,
        .code = "I000",
        .message = "Akkado compiler not yet implemented",
        .filename = std::string(filename),
        .location = {.line = 1, .column = 1, .offset = 0, .length = 0}
    });

    result.success = false;
    return result;
}

CompileResult compile_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        CompileResult result;
        result.diagnostics.push_back(Diagnostic{
            .severity = Severity::Error,
            .code = "E000",
            .message = "Could not open file: " + path,
            .filename = path,
            .location = {}
        });
        result.success = false;
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return compile(buffer.str(), path);
}

} // namespace akkado
