#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace akkado {

/// Source location for error reporting
struct SourceLocation {
    std::uint32_t line = 1;      // 1-based line number
    std::uint32_t column = 1;    // 1-based column number
    std::uint32_t offset = 0;    // 0-based byte offset
    std::uint32_t length = 0;    // Length of the span
};

/// Diagnostic severity levels
enum class Severity {
    Error,      // Compilation cannot continue
    Warning,    // Potential issue, compilation continues
    Info,       // Informational message
    Hint        // Suggestion for improvement
};

/// A single diagnostic message
struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code;           // Error code (e.g., "E001", "W002")
    std::string message;        // Human-readable message
    std::string filename;       // Source file name
    SourceLocation location;    // Location in source

    /// Related information (e.g., "previous declaration was here")
    struct Related {
        std::string message;
        std::string filename;
        SourceLocation location;
    };
    std::vector<Related> related;

    /// Suggested fix (for LSP quick-fix support)
    struct Fix {
        std::string description;
        std::string new_text;
        SourceLocation location;
    };
    std::optional<Fix> fix;
};

/// Format a diagnostic for terminal output
std::string format_diagnostic(const Diagnostic& diag, std::string_view source);

/// Format a diagnostic as JSON (for LSP/tooling)
std::string format_diagnostic_json(const Diagnostic& diag);

/// Check if any diagnostic is an error
bool has_errors(const std::vector<Diagnostic>& diagnostics);

} // namespace akkado
