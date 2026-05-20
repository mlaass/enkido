#include "akkado/akkado.hpp"
#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"
#include "akkado/analyzer.hpp"
#include "akkado/codegen.hpp"
#include "akkado/stdlib.hpp"
#include "akkado/source_map.hpp"
#include "akkado/import_scanner.hpp"
#include "akkado/file_resolver.hpp"
#include <cedar/vm/instruction.hpp>
#include <fstream>
#include <sstream>
#include <cstring>
#ifndef __EMSCRIPTEN__
#include <filesystem>
#endif

namespace akkado {

/// Count newlines in a string (for source map line offsets)
static std::size_t count_lines(std::string_view s) {
    std::size_t count = 1;
    for (char c : s) {
        if (c == '\n') ++count;
    }
    return count;
}

CompileResult compile(std::string_view source, std::string_view filename,
                     SampleRegistry* sample_registry,
                     const FileResolver* resolver,
                     bool lint_strict) {
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

    // Step 1: Resolve imports (if resolver provided)
    std::vector<ResolvedModule> resolved_modules;
    std::vector<NamespacedImport> namespaced_imports;
    std::string user_source;

    if (resolver) {
        auto import_result = resolve_imports(source, filename, *resolver);
        if (!import_result.success) {
            result.diagnostics = std::move(import_result.diagnostics);
            result.success = false;
            return result;
        }
        // Forward any warnings from import resolution
        result.diagnostics.insert(result.diagnostics.end(),
            import_result.diagnostics.begin(), import_result.diagnostics.end());
        resolved_modules = std::move(import_result.modules);
        namespaced_imports = std::move(import_result.namespaced_imports);
        user_source = std::move(import_result.root_source);
    } else {
        // No resolver: check if source contains imports (E505)
        auto user_imports = scan_imports(source);
        if (!user_imports.empty()) {
            for (const auto& dir : user_imports) {
                result.diagnostics.push_back(Diagnostic{
                    .severity = Severity::Error,
                    .code = "E505",
                    .message = "Import requires a file resolver (not available in this context)",
                    .filename = std::string(filename),
                    .location = {
                        .line = static_cast<std::uint32_t>(dir.line_number),
                        .column = 1,
                        .offset = static_cast<std::uint32_t>(dir.line_start),
                        .length = static_cast<std::uint32_t>(dir.line_length)
                    }
                });
            }
            result.success = false;
            return result;
        }
        user_source = std::string(source);
    }

    // Step 2: Build combined source
    // Order: stdlib + resolved modules (topo order) + user source
    std::string combined_source;
    SourceMap source_map;

    std::size_t total_size = STDLIB_SOURCE.size() + 1 + user_source.size();
    for (const auto& mod : resolved_modules) {
        total_size += mod.source.size() + 1;
    }
    combined_source.reserve(total_size);

    // Region 0: stdlib
    combined_source.append(STDLIB_SOURCE);
    combined_source.push_back('\n');
    const std::size_t stdlib_byte_length = STDLIB_SOURCE.size() + 1;
    source_map.add_region(std::string(STDLIB_FILENAME), 0, stdlib_byte_length, 0);

    std::size_t offset = stdlib_byte_length;
    std::size_t cumulative_lines = STDLIB_LINE_COUNT;

    // Regions 1..N-1: resolved modules (topo order, dependencies first)
    for (const auto& mod : resolved_modules) {
        combined_source.append(mod.source);
        combined_source.push_back('\n');
        std::size_t mod_byte_length = mod.source.size() + 1;
        source_map.add_region(mod.canonical_path, offset, mod_byte_length, cumulative_lines);
        cumulative_lines += count_lines(mod.source);
        offset += mod_byte_length;
    }

    // Region N: user source (with import lines blanked by scanner)
    combined_source.append(user_source);
    source_map.add_region(std::string(filename), offset, user_source.size(), cumulative_lines);

    // Phase 1: Lexing
    auto [tokens, lex_diags] = lex(combined_source, filename);
    source_map.adjust_all(lex_diags);
    result.diagnostics.insert(result.diagnostics.end(),
                              lex_diags.begin(), lex_diags.end());

    if (has_errors(lex_diags)) {
        result.success = false;
        return result;
    }

    // Phase 2: Parsing
    auto [ast, parse_diags] = parse(std::move(tokens), combined_source, filename,
                                     lint_strict);
    source_map.adjust_all(parse_diags);
    result.diagnostics.insert(result.diagnostics.end(),
                              parse_diags.begin(), parse_diags.end());

    if (has_errors(parse_diags)) {
        // Phase 2 records-system-unification: surface partial AST so callers
        // (e.g. shape-index tooling) can still inspect what was parsed.
        result.ast = std::make_shared<Ast>(std::move(ast));
        result.success = false;
        return result;
    }

    // Phase 3: Semantic Analysis
    std::vector<ModuleNamespace> namespaces;
    for (const auto& ns : namespaced_imports) {
        namespaces.push_back(ModuleNamespace{ns.canonical_path, ns.alias});
    }

    SemanticAnalyzer analyzer;
    auto analysis = analyzer.analyze(ast, filename, &source_map, namespaces);
    source_map.adjust_all(analysis.diagnostics);
    result.diagnostics.insert(result.diagnostics.end(),
                              analysis.diagnostics.begin(),
                              analysis.diagnostics.end());

    if (!analysis.success) {
        // Phase 2 records-system-unification: surface analyzer outputs so
        // partial-bound symbols (records, patterns) remain inspectable.
        result.ast = std::make_shared<Ast>(std::move(analysis.transformed_ast));
        result.symbols = std::move(analysis.symbols);
        result.success = false;
        return result;
    }

    // Phase 4: Code Generation
    CodeGenerator codegen;
    auto gen = codegen.generate(analysis.transformed_ast, analysis.symbols, filename, sample_registry, &source_map);
    source_map.adjust_all(gen.diagnostics);
    result.diagnostics.insert(result.diagnostics.end(),
                              gen.diagnostics.begin(),
                              gen.diagnostics.end());

    if (!gen.success) {
        // Phase 2 records-system-unification: surface analyzer outputs even
        // when codegen fails so shape-index tooling has a symbol table.
        result.ast = std::make_shared<Ast>(std::move(analysis.transformed_ast));
        result.symbols = std::move(analysis.symbols);
        result.success = false;
        return result;
    }

    // Convert instructions to byte array (full [ main | block bodies ] stream).
    result.bytecode.resize(gen.instructions.size() * sizeof(cedar::Instruction));
    std::memcpy(result.bytecode.data(), gen.instructions.data(),
                result.bytecode.size());

    // FOREACH_EVENT subprogram table (PRD L3): record the main/body boundary
    // and build the cedar::BlockEntry table the host hands to the VM.
    result.main_instruction_count = gen.main_instruction_count;
    result.block_table.clear();
    result.block_table.reserve(gen.subprograms.size());
    for (const auto& desc : gen.subprograms) {
        cedar::BlockEntry be{};
        be.offset = static_cast<std::uint16_t>(desc.offset);
        be.length = static_cast<std::uint16_t>(desc.body.size());
        be.frame_slot_count = desc.frame_slot_count;
        be.output_count = desc.output_count;
        be.param_base = desc.param_base;     // L2 BLOCK_CALL blocks; 0 for FOREACH
        be.body_output = desc.body_output;
        result.block_table.push_back(be);
    }

    // Copy source locations, adjusting offsets via source map
    result.source_locations = std::move(gen.source_locations);
    source_map.adjust_source_locations(result.source_locations);

    // Copy state initializations, adjusting pattern locations
    result.state_inits = std::move(gen.state_inits);
    source_map.adjust_state_inits(result.state_inits);

    // Copy required sample names for runtime loading
    result.required_samples = std::move(gen.required_samples);
    result.required_samples_extended = std::move(gen.required_samples_extended);
    result.scalar_sample_mappings = std::move(gen.scalar_sample_mappings);
    result.required_soundfonts = std::move(gen.required_soundfonts);
    result.required_midi_sources = std::move(gen.required_midi_sources);
    result.required_midi_cc_routes = std::move(gen.required_midi_cc_routes);
    result.required_wavetables = std::move(gen.required_wavetables);
    result.required_uris = std::move(gen.required_uris);
    result.required_input_sources = std::move(gen.required_input_sources);

    // Copy parameter declarations for UI generation
    result.param_decls = std::move(gen.param_decls);

    // Copy visualization declarations, adjusting offsets
    result.viz_decls = std::move(gen.viz_decls);
    source_map.adjust_viz_decls(result.viz_decls);

    // Copy builtin variable overrides
    result.builtin_var_overrides = std::move(gen.builtin_var_overrides);

    // Phase 2 records-system-unification: retain analyzer outputs for
    // downstream tooling (shape index, future LSP integrations).
    result.ast = std::make_shared<Ast>(std::move(analysis.transformed_ast));
    result.symbols = std::move(analysis.symbols);

    result.success = true;
    return result;
}

CompileResult compile_file(const std::string& path,
                          SampleRegistry* sample_registry,
                          const FileResolver* resolver,
                          bool lint_strict) {
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

#ifndef __EMSCRIPTEN__
    // If no resolver provided, create a default FilesystemResolver
    // using the file's parent directory
    if (!resolver) {
        std::filesystem::path p(path);
        auto dir = p.parent_path().string();
        if (dir.empty()) dir = ".";
        FilesystemResolver default_resolver({dir});
        return compile(buffer.str(), path, sample_registry, &default_resolver,
                       lint_strict);
    }
#endif

    return compile(buffer.str(), path, sample_registry, resolver, lint_strict);
}

} // namespace akkado
