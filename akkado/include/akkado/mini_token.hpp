#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include "diagnostics.hpp"

namespace akkado {

/// Token types for mini-notation patterns
/// These are distinct from main language tokens as mini-notation has different rules
enum class MiniTokenType : std::uint8_t {
    // End of pattern
    Eof,

    // Atoms
    PitchToken,     // c4, f#3, Bb5 (note with optional octave, defaults to 4)
    SampleToken,    // bd, sd, hh, cp:2 (sample name with optional variant)
    Rest,           // ~ or _
    Number,         // 0.5, 3, 4.0 (for modifiers and euclidean)

    // Groupings
    LBracket,       // [
    RBracket,       // ]
    LAngle,         // <
    RAngle,         // >
    LParen,         // (
    RParen,         // )
    LBrace,         // { (polymeter)
    RBrace,         // } (polymeter)
    Comma,          // ,

    // Modifiers
    Star,           // *n (speed up)
    Slash,          // /n (slow down)
    Colon,          // :n (duration) or :maj/:min (chord type)
    At,             // @n (weight)
    Bang,           // !n (repeat)
    Question,       // ?n (chance)
    Percent,        // %n (polymeter step count)

    // Choice operator
    Pipe,           // | (random choice)

    // Error token
    Error,
};

/// Convert mini token type to string for debugging
constexpr std::string_view mini_token_type_name(MiniTokenType type) {
    switch (type) {
        case MiniTokenType::Eof:         return "Eof";
        case MiniTokenType::PitchToken:  return "PitchToken";
        case MiniTokenType::SampleToken: return "SampleToken";
        case MiniTokenType::Rest:        return "Rest";
        case MiniTokenType::Number:      return "Number";
        case MiniTokenType::LBracket:    return "LBracket";
        case MiniTokenType::RBracket:    return "RBracket";
        case MiniTokenType::LAngle:      return "LAngle";
        case MiniTokenType::RAngle:      return "RAngle";
        case MiniTokenType::LParen:      return "LParen";
        case MiniTokenType::RParen:      return "RParen";
        case MiniTokenType::LBrace:      return "LBrace";
        case MiniTokenType::RBrace:      return "RBrace";
        case MiniTokenType::Comma:       return "Comma";
        case MiniTokenType::Star:        return "Star";
        case MiniTokenType::Slash:       return "Slash";
        case MiniTokenType::Colon:       return "Colon";
        case MiniTokenType::At:          return "At";
        case MiniTokenType::Bang:        return "Bang";
        case MiniTokenType::Question:    return "Question";
        case MiniTokenType::Percent:     return "Percent";
        case MiniTokenType::Pipe:        return "Pipe";
        case MiniTokenType::Error:       return "Error";
    }
    return "Unknown";
}

/// Pitch data for mini-notation (MIDI note)
struct MiniPitchData {
    std::uint8_t midi_note;    // MIDI note number (60 = C4)
    bool has_octave;           // Whether octave was explicit in source
};

/// Sample data for mini-notation
struct MiniSampleData {
    std::string name;          // Sample name (e.g., "bd", "sd")
    std::uint8_t variant = 0;  // Sample variant (e.g., 2 for "bd:2")
};

/// Token value for mini-notation
using MiniTokenValue = std::variant<
    std::monostate,     // For punctuation/operators
    double,             // For numbers
    MiniPitchData,      // For pitch tokens
    MiniSampleData,     // For sample tokens
    std::string         // For error messages
>;

/// A single token from the mini-notation lexer
struct MiniToken {
    MiniTokenType type = MiniTokenType::Eof;
    SourceLocation location{};
    std::string_view lexeme{};  // View into pattern string
    MiniTokenValue value{};

    /// Check if this is an error token
    [[nodiscard]] bool is_error() const { return type == MiniTokenType::Error; }

    /// Check if this is end of pattern
    [[nodiscard]] bool is_eof() const { return type == MiniTokenType::Eof; }

    /// Get numeric value (assumes type == Number)
    [[nodiscard]] double as_number() const {
        return std::get<double>(value);
    }

    /// Get pitch data (assumes type == PitchToken)
    [[nodiscard]] const MiniPitchData& as_pitch() const {
        return std::get<MiniPitchData>(value);
    }

    /// Get sample data (assumes type == SampleToken)
    [[nodiscard]] const MiniSampleData& as_sample() const {
        return std::get<MiniSampleData>(value);
    }

    /// Get error message (assumes type == Error)
    [[nodiscard]] const std::string& as_error() const {
        return std::get<std::string>(value);
    }
};

} // namespace akkado
