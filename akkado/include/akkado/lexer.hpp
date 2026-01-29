#pragma once

#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>
#include "token.hpp"
#include "diagnostics.hpp"

namespace akkado {

/// Lexer for the Akkado language
///
/// Converts source text into a stream of tokens. The lexer is designed to:
/// - Produce all tokens at once (for simplicity and error recovery)
/// - Generate detailed source locations for LSP integration
/// - Handle UTF-8 source correctly (treating multibyte chars as single units)
/// - Continue after errors to find as many issues as possible
class Lexer {
public:
    /// Construct a lexer for the given source
    /// @param source The source code to lex (must remain valid during lexing)
    /// @param filename The filename for error reporting
    explicit Lexer(std::string_view source, std::string_view filename = "<input>");

    /// Lex all tokens from the source
    /// @return Vector of tokens, ending with Eof token
    [[nodiscard]] std::vector<Token> lex_all();

    /// Get any diagnostics generated during lexing
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    /// Check if any errors occurred
    [[nodiscard]] bool has_errors() const;

private:
    // Source navigation
    [[nodiscard]] bool is_at_end() const;
    [[nodiscard]] char peek() const;
    [[nodiscard]] char peek_next() const;
    char advance();
    bool match(char expected);

    // Character classification
    [[nodiscard]] static bool is_digit(char c);
    [[nodiscard]] static bool is_alpha(char c);
    [[nodiscard]] static bool is_alphanumeric(char c);
    [[nodiscard]] static bool is_whitespace(char c);

    // Token creation
    Token make_token(TokenType type);
    Token make_token(TokenType type, TokenValue value);
    Token make_error_token(std::string_view message);

    // Lexing helpers
    void skip_whitespace();
    void skip_line_comment();
    Token lex_token();
    Token lex_number();
    Token lex_string(char quote);
    Token lex_identifier();
    std::optional<Token> try_lex_pitch_or_chord();  // Try to lex pitch literals: 'c4', 'f#3', etc.
    std::optional<Token> try_lex_strudel_chord();   // Try to lex Strudel-style chords: C4', F#m3', etc.

    // Keyword lookup
    [[nodiscard]] TokenType identifier_type(std::string_view text) const;

    // Error reporting
    void add_error(std::string_view message);
    void add_error(std::string_view message, SourceLocation loc);

    // Source tracking
    void update_location(char c);
    [[nodiscard]] SourceLocation current_location() const;

    std::string_view source_;
    std::string filename_;
    std::vector<Diagnostic> diagnostics_;

    // Current position
    std::uint32_t start_ = 0;    // Start of current token
    std::uint32_t current_ = 0;  // Current position
    std::uint32_t line_ = 1;     // Current line (1-based)
    std::uint32_t column_ = 1;   // Current column (1-based)

    // Token start position
    std::uint32_t token_line_ = 1;
    std::uint32_t token_column_ = 1;
};

/// Convenience function to lex source code
/// @param source The source code to lex
/// @param filename The filename for error reporting
/// @return Pair of tokens and diagnostics
std::pair<std::vector<Token>, std::vector<Diagnostic>>
lex(std::string_view source, std::string_view filename = "<input>");

} // namespace akkado
