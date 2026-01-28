#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
#include <span>
#include "diagnostics.hpp"
#include "codegen.hpp"  // For StateInitData

namespace akkado {

// Forward declaration
class SampleRegistry;

/// Akkado version information
struct Version {
    static constexpr int major = 0;
    static constexpr int minor = 1;
    static constexpr int patch = 0;

    static constexpr std::string_view string() { return "0.1.0"; }
};

/// Compilation result
struct CompileResult {
    bool success = false;
    std::vector<std::uint8_t> bytecode;
    std::vector<SourceLocation> source_locations;  // Parallel to bytecode instructions, tracks origin
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data for patterns
    std::vector<std::string> required_samples;  // Sample names used (for runtime loading)
    std::vector<ParamDecl> param_decls;  // Declared parameters for UI generation
};

/// Compile Akkado source code to Cedar bytecode
/// @param source The source code to compile
/// @param filename Optional filename for error reporting
/// @param sample_registry Optional sample registry for resolving sample names to IDs
/// @return Compilation result with bytecode and diagnostics
CompileResult compile(std::string_view source, std::string_view filename = "<input>",
                     SampleRegistry* sample_registry = nullptr);

/// Compile from file
/// @param path Path to the source file
/// @return Compilation result
CompileResult compile_file(const std::string& path);

} // namespace akkado
