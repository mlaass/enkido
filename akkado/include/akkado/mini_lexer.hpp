#pragma once

#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>
#include "mini_token.hpp"
#include "diagnostics.hpp"

namespace akkado {

/// Lexer for mini-notation patterns inside pat(), seq(), etc.
///
/// This is a separate lexer from the main Akkado lexer because mini-notation
/// has different lexical rules:
/// - No keywords (everything is either a pitch, sample, or operator)
/// - Different operator meanings (* is speed, not multiplication)
/// - Octave is optional for pitches (defaults to 4)
/// - Sample names are identifiers that don't look like pitches
///
/// Example patterns:
///   "bd sd bd sd"       - Simple drum pattern
///   "c4 e4 g4"          - Melodic sequence
///   "[bd sd] hh"        - Subdivision
///   "<c e g>"           - Alternating sequence
///   "bd*2"              - Speed modifier
///   "bd(3,8)"           - Euclidean rhythm
class MiniLexer {
public:
    /// Construct a mini-lexer for a pattern string
    /// @param pattern The pattern string content (without quotes)
    /// @param base_location Location of the pattern string in source for error reporting
    /// @param mode Parse mode (Auto/Note/Sample/Chord/Value/Curve) — see MiniParseMode
    explicit MiniLexer(std::string_view pattern, SourceLocation base_location = {},
                       MiniParseMode mode = MiniParseMode::Auto);

    /// Backward-compat constructor (replaces sample_only/curve_mode bools).
    explicit MiniLexer(std::string_view pattern, SourceLocation base_location,
                       bool sample_only, bool curve_mode);

    /// Lex all tokens from the pattern
    /// @return Vector of tokens, ending with Eof token
    [[nodiscard]] std::vector<MiniToken> lex_all();

    /// Get any diagnostics generated during lexing
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    /// Check if any errors occurred
    [[nodiscard]] bool has_errors() const;

private:
    // Source navigation
    [[nodiscard]] bool is_at_end() const;
    [[nodiscard]] char peek() const;
    [[nodiscard]] char peek_next() const;
    [[nodiscard]] char peek_ahead(std::size_t n) const;
    char advance();
    bool match(char expected);

    // Character classification
    [[nodiscard]] static bool is_digit(char c);
    [[nodiscard]] static bool is_alpha(char c);
    [[nodiscard]] static bool is_pitch_letter(char c);
    [[nodiscard]] static bool is_accidental(char c);
    [[nodiscard]] static bool is_whitespace(char c);

    // Token creation
    MiniToken make_token(MiniTokenType type);
    MiniToken make_token(MiniTokenType type, MiniTokenValue value);
    MiniToken make_error_token(std::string_view message);

    // Lexing methods
    void skip_whitespace();
    MiniToken lex_token();
    MiniToken lex_number();
    MiniToken lex_pitch();          // Character-by-character pitch parsing (handles ^v+x modifiers)
    MiniToken lex_sample_only();    // For sample_only mode
    MiniToken lex_value_atom();     // For value mode: numeric literal (incl. negative, scientific)
    MiniToken lex_note_atom();      // For note mode: numeric MIDI note (e.g., "60" → midi_note 60)

    // Pitch detection
    [[nodiscard]] bool looks_like_pitch() const;
    [[nodiscard]] std::uint8_t parse_pitch_to_midi(char note, int accidental, int octave) const;

    // Phase 2 PRD: scan a record-suffix `{key:value, ...}` immediately after
    // a note token (no whitespace). Caller has already produced the note
    // token; this consumes from `{` to `}` and returns the parsed properties.
    // If `{` is not present (or is followed by anything that doesn't look
    // like `key:value`), returns an empty vector and consumes nothing.
    std::vector<std::pair<std::string, float>> try_lex_record_suffix();

    // Chord symbol detection
    [[nodiscard]] std::optional<MiniToken> try_lex_chord_symbol();

    // Location helpers
    [[nodiscard]] SourceLocation current_location() const;

    std::string_view pattern_;
    SourceLocation base_location_;
    std::vector<Diagnostic> diagnostics_;
    MiniParseMode mode_ = MiniParseMode::Auto;
    bool sample_only_ = false;   // When true, skip pitch detection (Sample/Chord mode)
    bool curve_mode_ = false;    // When true, lex curve notation characters
    bool value_mode_ = false;    // When true, atoms must be numeric literals (v"…")
    bool note_mode_ = false;     // When true, bare ints are MIDI notes (n"…")

    // True iff the most recently emitted token was a modifier (`*`, `/`, `@`, `!`, `?`).
    // The next call to lex_token() consults this so a digit-leading token following a
    // modifier is lexed as a plain Number even inside `v"…"` / `n"…"`, instead of being
    // captured by lex_value_atom / lex_note_atom. Cleared at the top of lex_token().
    bool last_was_modifier_ = false;

    // Current position
    std::uint32_t start_ = 0;    // Start of current token
    std::uint32_t current_ = 0;  // Current position
    std::uint32_t column_ = 1;   // Column within pattern (1-based)
};

/// Convenience function to lex a mini-notation pattern
/// @param pattern The pattern string content
/// @param base_location Location for error reporting
/// @param mode Parse mode (Auto/Note/Sample/Chord/Value/Curve)
/// @return Pair of tokens and diagnostics
std::pair<std::vector<MiniToken>, std::vector<Diagnostic>>
lex_mini(std::string_view pattern, SourceLocation base_location = {},
         MiniParseMode mode = MiniParseMode::Auto);

/// Backward-compat overload that maps the old bool flags to a MiniParseMode.
std::pair<std::vector<MiniToken>, std::vector<Diagnostic>>
lex_mini(std::string_view pattern, SourceLocation base_location,
         bool sample_only, bool curve_mode);

} // namespace akkado
