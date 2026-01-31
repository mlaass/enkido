#include "akkado/diagnostics.hpp"
#include <sstream>
#include <algorithm>

namespace akkado {

namespace {

std::string_view severity_string(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Info:    return "info";
        case Severity::Hint:    return "hint";
    }
    return "unknown";
}

std::string_view severity_color(Severity s) {
    switch (s) {
        case Severity::Error:   return "\033[1;31m"; // Bold red
        case Severity::Warning: return "\033[1;33m"; // Bold yellow
        case Severity::Info:    return "\033[1;36m"; // Bold cyan
        case Severity::Hint:    return "\033[1;32m"; // Bold green
    }
    return "";
}

constexpr std::string_view RESET = "\033[0m";
constexpr std::string_view BOLD = "\033[1m";

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// Get the line from source at the given line number (1-based)
std::string_view get_line(std::string_view source, std::uint32_t line_num) {
    std::uint32_t current_line = 1;
    std::size_t line_start = 0;

    for (std::size_t i = 0; i < source.size(); ++i) {
        if (current_line == line_num) {
            line_start = i;
            break;
        }
        if (source[i] == '\n') {
            ++current_line;
        }
    }

    if (current_line != line_num) {
        return {};
    }

    auto line_end = source.find('\n', line_start);
    if (line_end == std::string_view::npos) {
        line_end = source.size();
    }

    return source.substr(line_start, line_end - line_start);
}

} // namespace

std::string format_diagnostic(const Diagnostic& diag, std::string_view source) {
    std::ostringstream out;

    // Header: filename:line:column: severity[code]: message
    out << BOLD << diag.filename << ":"
        << diag.location.line << ":"
        << diag.location.column << ": " << RESET;

    out << severity_color(diag.severity) << severity_string(diag.severity);
    if (!diag.code.empty()) {
        out << "[" << diag.code << "]";
    }
    out << RESET << ": " << BOLD << diag.message << RESET << "\n";

    // Source line with caret
    if (!source.empty() && diag.location.line > 0) {
        auto line = get_line(source, diag.location.line);
        if (!line.empty()) {
            // Line number gutter
            out << "    " << diag.location.line << " | " << line << "\n";

            // Caret line
            out << "      | ";
            for (std::uint32_t i = 1; i < diag.location.column; ++i) {
                out << ' ';
            }
            out << severity_color(diag.severity) << "^";
            for (std::uint32_t i = 1; i < diag.location.length && i < 80; ++i) {
                out << "~";
            }
            out << RESET << "\n";
        }
    }

    // Related information
    for (const auto& rel : diag.related) {
        out << BOLD << rel.filename << ":"
            << rel.location.line << ":"
            << rel.location.column << ": " << RESET
            << "note: " << rel.message << "\n";
    }

    // Suggested fix
    if (diag.fix) {
        out << "  = help: " << diag.fix->description << "\n";
    }

    return out.str();
}

std::string format_diagnostic_json(const Diagnostic& diag) {
    std::ostringstream out;

    out << R"({"severity":")" << severity_string(diag.severity) << R"(",)";
    out << R"("code":")" << escape_json(diag.code) << R"(",)";
    out << R"("message":")" << escape_json(diag.message) << R"(",)";
    out << R"("file":")" << escape_json(diag.filename) << R"(",)";
    out << R"("range":{"start":{"line":)" << (diag.location.line - 1)
        << R"(,"character":)" << (diag.location.column - 1) << R"(},)";
    out << R"("end":{"line":)" << (diag.location.line - 1)
        << R"(,"character":)" << (diag.location.column - 1 + diag.location.length) << R"(}}})";

    return out.str();
}

bool has_errors(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

} // namespace akkado
