#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "diagnostics.hpp"

namespace akkado {

/// Mini-notation parse mode (PRD prd-patterns-as-scalar-values §5.2).
/// Selects per-atom rules for mini-notation parsing. Subsumes the legacy
/// `sample_only` and `curve_mode` boolean flags into a single enum.
enum class MiniParseMode : std::uint8_t {
    Note,    // n"…" — note names + bare MIDI ints both → mtof
    Sample,  // s"…" — atom = sample name (replaces `sample_only = true`)
    Chord,   // c"…" — atom = chord symbol
    Value,   // v"…" — atom must be numeric literal; raw value, no mtof
    Curve,   // t"…" — timeline / breakpoint mode (replaces `curve_mode = true`)
};

constexpr const char* mini_parse_mode_name(MiniParseMode m) {
    switch (m) {
        case MiniParseMode::Note:   return "note";
        case MiniParseMode::Sample: return "sample";
        case MiniParseMode::Chord:  return "chord";
        case MiniParseMode::Value:  return "value";
        case MiniParseMode::Curve:  return "curve";
    }
    return "?";
}

/// Token types for mini-notation patterns
/// These are distinct from main language tokens as mini-notation has different rules
enum class MiniTokenType : std::uint8_t {
    // End of pattern
    Eof,

    // Atoms
    PitchToken,     // c4, f#3, Bb5 (note with optional octave, defaults to 4)
    SampleToken,    // bd, sd, hh, cp:2 (sample name with optional variant)
    ChordToken,     // Am, C7, Fmaj7, G (chord symbol without octave)
    Rest,           // ~ (silent step)
    Elongate,       // _ (extend previous note - Tidal-compatible)
    CurveLevel,     // _, ., -, ^, ' (value level atom in curve mode)
    CurveRamp,      // /, \ (ramp atom in curve mode)
    CurveSmooth,    // ~ (smooth modifier prefix in curve mode)
    ValueAtom,      // 0, 0.5, -0.5, 1e3 (for v"…" value-mode atoms)
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
    Colon,          // :n (sample variant) - handled in lexer for samples
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
        case MiniTokenType::ChordToken:  return "ChordToken";
        case MiniTokenType::Rest:        return "Rest";
        case MiniTokenType::Elongate:    return "Elongate";
        case MiniTokenType::CurveLevel:  return "CurveLevel";
        case MiniTokenType::CurveRamp:   return "CurveRamp";
        case MiniTokenType::CurveSmooth: return "CurveSmooth";
        case MiniTokenType::ValueAtom:   return "ValueAtom";
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
    float velocity = 1.0f;     // 0.0-1.0, from :vel suffix (e.g., c4:0.8)
    std::int8_t micro_offset = 0;  // Microtonal step offset (^=+1, v=-1, stacking for larger)
    // Phase 2 PRD: record-suffix properties from `c4{vel:0.8, bend:0.2}`.
    // Lexer parses {key:value, ...} immediately following a note token.
    std::vector<std::pair<std::string, float>> properties;
};

/// Sample data for mini-notation
struct MiniSampleData {
    std::string name;          // Sample name (e.g., "bd", "sd")
    std::uint8_t variant = 0;  // Sample variant (e.g., 2 for "bd:2")
    std::string bank;          // Bank name (empty = default)
    std::vector<std::pair<std::string, float>> properties;  // record suffix
};

/// Chord data for mini-notation (chord symbol like Am, C7, Fmaj7)
struct MiniChordData {
    std::string root;                      // Root note name: "A", "C#", "Bb"
    std::string quality;                   // Chord quality: "", "m", "7", "maj7", etc.
    std::uint8_t root_midi;                // MIDI note of root (default octave 4)
    std::vector<std::int8_t> intervals;    // Semitone intervals from root
    float velocity = 1.0f;                 // 0.0-1.0, from :vel suffix (e.g., Am:0.5)
    std::vector<std::pair<std::string, float>> properties;  // record suffix
};

/// Curve level data for curve-mode notation (value level atom)
struct MiniCurveLevelData {
    float value;  // 0.0, 0.25, 0.5, 0.75, or 1.0
};

/// Token value for mini-notation
using MiniTokenValue = std::variant<
    std::monostate,     // For punctuation/operators
    double,             // For numbers
    MiniPitchData,      // For pitch tokens
    MiniSampleData,     // For sample tokens
    MiniChordData,      // For chord tokens
    MiniCurveLevelData, // For curve level tokens
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

    /// Get chord data (assumes type == ChordToken)
    [[nodiscard]] const MiniChordData& as_chord() const {
        return std::get<MiniChordData>(value);
    }

    /// Get curve level data (assumes type == CurveLevel)
    [[nodiscard]] const MiniCurveLevelData& as_curve_level() const {
        return std::get<MiniCurveLevelData>(value);
    }

    /// Get error message (assumes type == Error)
    [[nodiscard]] const std::string& as_error() const {
        return std::get<std::string>(value);
    }
};

} // namespace akkado
