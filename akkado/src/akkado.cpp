#include "akkado/akkado.hpp"
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"
#include "akkado/analyzer.hpp"
#include "akkado/codegen.hpp"
#include <cedar/vm/instruction.hpp>
#include <fstream>
#include <sstream>
#include <cstring>

namespace akkado {

CompileResult compile(std::string_view source, std::string_view filename,
                     SampleRegistry* sample_registry) {
    CompileResult result;

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

    // Phase 1: Lexing
    auto [tokens, lex_diags] = lex(source, filename);
    result.diagnostics.insert(result.diagnostics.end(),
                              lex_diags.begin(), lex_diags.end());

    if (has_errors(lex_diags)) {
        result.success = false;
        return result;
    }

    // Phase 2: Parsing
    auto [ast, parse_diags] = parse(std::move(tokens), source, filename);
    result.diagnostics.insert(result.diagnostics.end(),
                              parse_diags.begin(), parse_diags.end());

    if (has_errors(parse_diags)) {
        result.success = false;
        return result;
    }

    // Phase 3: Semantic Analysis
    SemanticAnalyzer analyzer;
    auto analysis = analyzer.analyze(ast, filename);
    result.diagnostics.insert(result.diagnostics.end(),
                              analysis.diagnostics.begin(),
                              analysis.diagnostics.end());

    if (!analysis.success) {
        result.success = false;
        return result;
    }

    // Phase 4: Code Generation
    CodeGenerator codegen;
    auto gen = codegen.generate(analysis.transformed_ast, analysis.symbols, filename, sample_registry);
    result.diagnostics.insert(result.diagnostics.end(),
                              gen.diagnostics.begin(),
                              gen.diagnostics.end());

    if (!gen.success) {
        result.success = false;
        return result;
    }

    // Convert instructions to byte array
    result.bytecode.resize(gen.instructions.size() * sizeof(cedar::Instruction));
    std::memcpy(result.bytecode.data(), gen.instructions.data(),
                result.bytecode.size());

    // Copy state initializations for patterns
    result.state_inits = std::move(gen.state_inits);

    // Copy required sample names for runtime loading
    result.required_samples = std::move(gen.required_samples);

    result.success = true;
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
