#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "diagnostics.hpp"

namespace akkado {

/// Token types for the Akkado language
enum class TokenType : std::uint8_t {
    // End of file
    Eof,

    // Literals
    Number,         // 42, 3.14, -1.5
    String,         // "hello"
    Identifier,     // foo, bar_baz
    PitchLit,       // 'c4', 'f#3', 'Bb5'
    ChordLit,       // 'c4:maj', 'a3:min7'

    // Keywords
    True,           // true
    False,          // false
    Post,           // post
    Match,          // match
    Fn,             // fn

    // Pattern types (used with mini-notation)
    Pat,            // pat(...)
    Seq,            // seq(...)
    Timeline,       // timeline(...)
    Note,           // note(...)

    // Operators
    Plus,           // +
    Minus,          // -
    Star,           // *
    Slash,          // /
    Caret,          // ^
    Dot,            // . (method call)
    Pipe,           // |>
    Equals,         // =
    Arrow,          // ->

    // Comparison (for potential future use)
    Less,           // <
    Greater,        // >
    LessEqual,      // <=
    GreaterEqual,   // >=
    EqualEqual,     // ==
    BangEqual,      // !=

    // Delimiters
    LParen,         // (
    RParen,         // )
    LBracket,       // [
    RBracket,       // ]
    LBrace,         // {
    RBrace,         // }
    Comma,          // ,
    Colon,          // :
    Semicolon,      // ;

    // Special
    Hole,           // %
    At,             // @ (for weight modifier in mini-notation)
    Bang,           // ! (for repeat modifier)
    Question,       // ? (for chance modifier)
    Tilde,          // ~ (rest in mini-notation)
    Underscore,     // _ (rest in mini-notation)

    // Mini-notation specific (lexed inside pattern strings)
    MiniString,     // The raw mini-notation string content

    // Error token (lexer encountered invalid input)
    Error,
};

/// Convert token type to string for debugging
constexpr std::string_view token_type_name(TokenType type) {
    switch (type) {
        case TokenType::Eof:          return "Eof";
        case TokenType::Number:       return "Number";
        case TokenType::String:       return "String";
        case TokenType::Identifier:   return "Identifier";
        case TokenType::PitchLit:     return "PitchLit";
        case TokenType::ChordLit:     return "ChordLit";
        case TokenType::True:         return "True";
        case TokenType::False:        return "False";
        case TokenType::Post:         return "Post";
        case TokenType::Match:        return "Match";
        case TokenType::Fn:           return "Fn";
        case TokenType::Pat:          return "Pat";
        case TokenType::Seq:          return "Seq";
        case TokenType::Timeline:     return "Timeline";
        case TokenType::Note:         return "Note";
        case TokenType::Plus:         return "Plus";
        case TokenType::Minus:        return "Minus";
        case TokenType::Star:         return "Star";
        case TokenType::Slash:        return "Slash";
        case TokenType::Caret:        return "Caret";
        case TokenType::Dot:          return "Dot";
        case TokenType::Pipe:         return "Pipe";
        case TokenType::Equals:       return "Equals";
        case TokenType::Arrow:        return "Arrow";
        case TokenType::Less:         return "Less";
        case TokenType::Greater:      return "Greater";
        case TokenType::LessEqual:    return "LessEqual";
        case TokenType::GreaterEqual: return "GreaterEqual";
        case TokenType::EqualEqual:   return "EqualEqual";
        case TokenType::BangEqual:    return "BangEqual";
        case TokenType::LParen:       return "LParen";
        case TokenType::RParen:       return "RParen";
        case TokenType::LBracket:     return "LBracket";
        case TokenType::RBracket:     return "RBracket";
        case TokenType::LBrace:       return "LBrace";
        case TokenType::RBrace:       return "RBrace";
        case TokenType::Comma:        return "Comma";
        case TokenType::Colon:        return "Colon";
        case TokenType::Semicolon:    return "Semicolon";
        case TokenType::Hole:         return "Hole";
        case TokenType::At:           return "At";
        case TokenType::Bang:         return "Bang";
        case TokenType::Question:     return "Question";
        case TokenType::Tilde:        return "Tilde";
        case TokenType::Underscore:   return "Underscore";
        case TokenType::MiniString:   return "MiniString";
        case TokenType::Error:        return "Error";
    }
    return "Unknown";
}

/// Numeric value (integer or float)
struct NumericValue {
    double value;
    bool is_integer;
};

/// Pitch value (MIDI note number)
struct PitchValue {
    std::uint8_t midi_note;
};

/// Chord value (root MIDI note + intervals)
struct ChordValue {
    std::uint8_t root_midi;
    std::vector<std::int8_t> intervals;
};

/// Token value - can be a number, string, pitch, chord, or nothing
using TokenValue = std::variant<std::monostate, NumericValue, std::string, PitchValue, ChordValue>;

/// A single token from the lexer
struct Token {
    TokenType type = TokenType::Eof;
    SourceLocation location{};
    std::string_view lexeme{};  // View into source (valid while source exists)
    TokenValue value{};         // Parsed value for literals

    /// Check if this is an error token
    [[nodiscard]] bool is_error() const { return type == TokenType::Error; }

    /// Check if this is end of file
    [[nodiscard]] bool is_eof() const { return type == TokenType::Eof; }

    /// Get numeric value (assumes type == Number)
    [[nodiscard]] double as_number() const {
        return std::get<NumericValue>(value).value;
    }

    /// Get string value (assumes type == String or Identifier)
    [[nodiscard]] const std::string& as_string() const {
        return std::get<std::string>(value);
    }

    /// Get pitch MIDI note (assumes type == PitchLit)
    [[nodiscard]] std::uint8_t as_pitch() const {
        return std::get<PitchValue>(value).midi_note;
    }

    /// Get chord value (assumes type == ChordLit)
    [[nodiscard]] const ChordValue& as_chord() const {
        return std::get<ChordValue>(value);
    }
};

} // namespace akkado
