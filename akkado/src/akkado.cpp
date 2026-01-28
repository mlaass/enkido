#include "akkado/akkado.hpp"
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"
#include "akkado/analyzer.hpp"
#include "akkado/codegen.hpp"
#include "akkado/stdlib.hpp"
#include <cedar/vm/instruction.hpp>
#include <fstream>
#include <sstream>
#include <cstring>

namespace akkado {

/// Adjust diagnostic locations to account for stdlib prepended to user source.
/// Diagnostics in user code get line numbers adjusted back to user's view.
/// Diagnostics in stdlib get their filename changed to STDLIB_FILENAME.
static void adjust_diagnostics(std::vector<Diagnostic>& diagnostics,
                                std::size_t stdlib_byte_offset,
                                std::size_t stdlib_line_count,
                                std::string_view user_filename) {
    for (auto& diag : diagnostics) {
        if (diag.location.offset < stdlib_byte_offset) {
            // Error is in stdlib - mark it as such
            diag.filename = std::string(STDLIB_FILENAME);
        } else {
            // Error is in user code - adjust line/offset back to user's view
            // stdlib_line_count is the number of lines in stdlib, and we add a newline
            // after stdlib, so user code starts on line (stdlib_line_count + 1).
            // To convert combined line L to user line: user_line = L - stdlib_line_count
            diag.location.line -= static_cast<std::uint32_t>(stdlib_line_count);
            diag.location.offset -= static_cast<std::uint32_t>(stdlib_byte_offset);
            diag.filename = std::string(user_filename);
        }

        // Also adjust related diagnostics
        for (auto& rel : diag.related) {
            if (rel.location.offset < stdlib_byte_offset) {
                rel.filename = std::string(STDLIB_FILENAME);
            } else {
                rel.location.line -= static_cast<std::uint32_t>(stdlib_line_count);
                rel.location.offset -= static_cast<std::uint32_t>(stdlib_byte_offset);
                rel.filename = std::string(user_filename);
            }
        }

        // Adjust fix location if present
        if (diag.fix) {
            if (diag.fix->location.offset >= stdlib_byte_offset) {
                diag.fix->location.line -= static_cast<std::uint32_t>(stdlib_line_count);
                diag.fix->location.offset -= static_cast<std::uint32_t>(stdlib_byte_offset);
            }
        }
    }
}

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

    // Prepend stdlib to user source
    std::string combined_source;
    combined_source.reserve(STDLIB_SOURCE.size() + 1 + source.size());
    combined_source.append(STDLIB_SOURCE);
    combined_source.push_back('\n');
    combined_source.append(source);

    const std::size_t stdlib_byte_offset = STDLIB_SOURCE.size() + 1;  // +1 for the newline

    // Phase 1: Lexing
    auto [tokens, lex_diags] = lex(combined_source, filename);
    adjust_diagnostics(lex_diags, stdlib_byte_offset, STDLIB_LINE_COUNT, filename);
    result.diagnostics.insert(result.diagnostics.end(),
                              lex_diags.begin(), lex_diags.end());

    if (has_errors(lex_diags)) {
        result.success = false;
        return result;
    }

    // Phase 2: Parsing
    auto [ast, parse_diags] = parse(std::move(tokens), combined_source, filename);
    adjust_diagnostics(parse_diags, stdlib_byte_offset, STDLIB_LINE_COUNT, filename);
    result.diagnostics.insert(result.diagnostics.end(),
                              parse_diags.begin(), parse_diags.end());

    if (has_errors(parse_diags)) {
        result.success = false;
        return result;
    }

    // Phase 3: Semantic Analysis
    SemanticAnalyzer analyzer;
    auto analysis = analyzer.analyze(ast, filename);
    adjust_diagnostics(analysis.diagnostics, stdlib_byte_offset, STDLIB_LINE_COUNT, filename);
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
    adjust_diagnostics(gen.diagnostics, stdlib_byte_offset, STDLIB_LINE_COUNT, filename);
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

    // Copy state initializations for patterns, adjusting offsets for stdlib
    result.state_inits = std::move(gen.state_inits);
    for (auto& init : result.state_inits) {
        if (init.pattern_location.offset >= stdlib_byte_offset) {
            init.pattern_location.offset -= static_cast<std::uint32_t>(stdlib_byte_offset);
        }
    }

    // Copy required sample names for runtime loading
    result.required_samples = std::move(gen.required_samples);

    // Copy parameter declarations for UI generation
    result.param_decls = std::move(gen.param_decls);

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
