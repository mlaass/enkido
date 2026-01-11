#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
#include <span>
#include "diagnostics.hpp"
#include "codegen.hpp"  // For StateInitData

namespace akkado {

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
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data for patterns
};

/// Compile Akkado source code to Cedar bytecode
/// @param source The source code to compile
/// @param filename Optional filename for error reporting
/// @return Compilation result with bytecode and diagnostics
CompileResult compile(std::string_view source, std::string_view filename = "<input>");

/// Compile from file
/// @param path Path to the source file
/// @return Compilation result
CompileResult compile_file(const std::string& path);

} // namespace akkado
