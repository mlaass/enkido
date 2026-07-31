#pragma once

// Hardening PRD Phase 5 (PRD-8): lexing primitives shared by the full
// Lexer and the mini-notation MiniLexer. Namespace divergence from the
// PRD spec (`akkado::lex`): `akkado::lex` is already the convenience
// *function* wrapping Lexer, so the namespace is `lex_primitives`.

#include <cstdint>
#include <string_view>

namespace akkado::lex_primitives {

// Character classifiers (shared verbatim by both lexers).
[[nodiscard]] constexpr bool is_digit(char c) {
    return c >= '0' && c <= '9';
}
[[nodiscard]] constexpr bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}
[[nodiscard]] constexpr bool is_alpha_numeric(char c) {
    return is_alpha(c) || is_digit(c);
}
[[nodiscard]] constexpr bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
[[nodiscard]] constexpr bool is_pitch_letter(char c) {
    return (c >= 'a' && c <= 'g') || (c >= 'A' && c <= 'G');
}

/// Source cursor shared by Lexer and MiniLexer. Owns position + line/
/// column tracking; `advance()` bumps line on '\n' (the F8-correct
/// semantics both lexers already had). Derived lexers access the
/// protected members directly for rollback paths.
class CursorBase {
public:
    explicit CursorBase(std::string_view source) : source_(source) {}

    [[nodiscard]] bool is_at_end() const { return current_ >= source_.size(); }
    [[nodiscard]] char peek() const {
        return is_at_end() ? '\0' : source_[current_];
    }
    [[nodiscard]] char peek_next() const {
        return current_ + 1 >= source_.size() ? '\0' : source_[current_ + 1];
    }
    [[nodiscard]] char peek_ahead(std::size_t n) const {
        return current_ + n >= source_.size() ? '\0' : source_[current_ + n];
    }
    char advance() {
        char c = source_[current_++];
        if (c == '\n') {
            line_++;
            column_ = 1;
        } else {
            column_++;
        }
        return c;
    }
    bool match(char expected) {
        if (is_at_end() || source_[current_] != expected) return false;
        advance();
        return true;
    }

    [[nodiscard]] std::uint32_t line() const { return line_; }
    [[nodiscard]] std::uint32_t column() const { return column_; }
    [[nodiscard]] std::uint32_t current() const { return current_; }
    [[nodiscard]] std::uint32_t start() const { return start_; }
    void mark_start() { start_ = current_; }

    /// Slice of the source from `from` (inclusive) up to the cursor.
    [[nodiscard]] std::string_view slice(std::size_t from) const {
        return source_.substr(from, current_ - from);
    }

protected:
    std::string_view source_;
    std::uint32_t start_ = 0;    // Start of current token
    std::uint32_t current_ = 0;  // Current position
    std::uint32_t line_ = 1;     // Current line (1-based)
    std::uint32_t column_ = 1;   // Current column (1-based)
};

struct NumericScanOpts {
    bool allow_sign = false;      // consume a leading +/- at the cursor
    bool allow_exponent = false;  // e/E exponent, validated by pure lookahead
                                  // (an invalid exponent is never consumed)
    bool greedy_dot = false;      // consume a post-digit '.' even when no
                                  // digit follows ("1." → 1.0); when false
                                  // the dot is left for the next token
    bool seen_dot = false;        // caller already consumed a leading '.'
};

struct NumericScanResult {
    double value = 0.0;
    bool is_integer = true;  // no fractional dot and no exponent
    bool ok = false;         // text from parse_from parsed as a number
};

/// Continue scanning a numeric literal at the cursor and parse the text
/// from `parse_from` (the token start — it may include sign / leading
/// dot / digits the caller already consumed).
NumericScanResult scan_number(CursorBase& cur, std::uint32_t parse_from,
                              NumericScanOpts opts);

/// Scan an optional `:n` velocity suffix (`c4:0.8`, `Am:0.5`, `60:.7`).
/// Returns 1.0f (consuming nothing) when no suffix is present;
/// otherwise the value clamped to [0, 1].
float scan_velocity_suffix(CursorBase& cur);

/// Note letter + accidental offset + octave → MIDI note, clamped to
/// 0..127. (The PRD sketched a string_view-parsing signature, but both
/// call sites scan char-by-char with lexer-specific lookahead — this is
/// the actually-shared tail.)
[[nodiscard]] std::uint8_t pitch_to_midi(char note_letter, int accidental,
                                         int octave);

} // namespace akkado::lex_primitives
