#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "diagnostics.hpp"
#include "string_interner.hpp"

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

    // Keywords
    True,           // true
    False,          // false
    Match,          // match
    Fn,             // fn
    As,             // as (pipe binding)
    Const,          // const
    Import,         // import
    // Note: parameter type annotations (Signal/Number/Pattern/Record/Array/
    // String/Function/Stream) are NOT keyword tokens — they lex as
    // Identifier and are resolved contextually in the parser.

    // Pattern types (used with mini-notation)
    Timeline,       // t"..." (timeline curve notation)
    ValuePat,       // v"…" — numeric scalar pattern (no mtof)
    NotePat,        // n"…" — note name + bare-MIDI pattern
    SamplePat,      // s"…" — sample-name pattern
    ChordPat,       // c"…" — chord-symbol pattern

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
    Diamond,        // <> (bus-routing statement terminator)

    // Comparison
    Less,           // <
    Greater,        // >
    LessEqual,      // <=
    GreaterEqual,   // >=
    EqualEqual,     // ==
    BangEqual,      // !=

    // Logic
    AndAnd,         // &&
    OrOr,           // ||

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
    Hash,           // # (annotation prefix, e.g. #inline)
    Hole,           // %
    At,             // @ (for weight modifier in mini-notation)
    Bang,           // ! (for repeat modifier)
    Question,       // ? (for chance modifier)
    Tilde,          // ~ (rest in mini-notation)
    Underscore,     // _ (rest in mini-notation / partial application placeholder)
    DotDot,         // .. (range in match patterns)
    DotDotDot,      // ... (variadic rest parameter prefix)

    // Directives
    Directive,      // $name (compiler directive like $polyphony)

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
        case TokenType::True:         return "True";
        case TokenType::False:        return "False";
        case TokenType::Match:        return "Match";
        case TokenType::Fn:           return "Fn";
        case TokenType::As:           return "As";
        case TokenType::Const:        return "Const";
        case TokenType::Import:       return "Import";
        case TokenType::Timeline:     return "Timeline";
        case TokenType::ValuePat:     return "ValuePat";
        case TokenType::NotePat:      return "NotePat";
        case TokenType::SamplePat:    return "SamplePat";
        case TokenType::ChordPat:     return "ChordPat";
        case TokenType::Plus:         return "Plus";
        case TokenType::Minus:        return "Minus";
        case TokenType::Star:         return "Star";
        case TokenType::Slash:        return "Slash";
        case TokenType::Caret:        return "Caret";
        case TokenType::Dot:          return "Dot";
        case TokenType::Pipe:         return "Pipe";
        case TokenType::Equals:       return "Equals";
        case TokenType::Arrow:        return "Arrow";
        case TokenType::Diamond:      return "Diamond";
        case TokenType::Less:         return "Less";
        case TokenType::Greater:      return "Greater";
        case TokenType::LessEqual:    return "LessEqual";
        case TokenType::GreaterEqual: return "GreaterEqual";
        case TokenType::EqualEqual:   return "EqualEqual";
        case TokenType::BangEqual:    return "BangEqual";
        case TokenType::AndAnd:       return "AndAnd";
        case TokenType::OrOr:         return "OrOr";
        case TokenType::LParen:       return "LParen";
        case TokenType::RParen:       return "RParen";
        case TokenType::LBracket:     return "LBracket";
        case TokenType::RBracket:     return "RBracket";
        case TokenType::LBrace:       return "LBrace";
        case TokenType::RBrace:       return "RBrace";
        case TokenType::Comma:        return "Comma";
        case TokenType::Colon:        return "Colon";
        case TokenType::Semicolon:    return "Semicolon";
        case TokenType::Hash:         return "Hash";
        case TokenType::Hole:         return "Hole";
        case TokenType::At:           return "At";
        case TokenType::Bang:         return "Bang";
        case TokenType::Question:     return "Question";
        case TokenType::Tilde:        return "Tilde";
        case TokenType::Underscore:   return "Underscore";
        case TokenType::DotDot:       return "DotDot";
        case TokenType::DotDotDot:    return "DotDotDot";
        case TokenType::Directive:    return "Directive";
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

/// PRD prd-parser-codegen-correctness.md Phase 5 (F12): wrapper around
/// raw literal text so the TokenValue variant distinguishes
/// identifier-like tokens (carry SymbolId; identifier equality is
/// id == id) from genuine string-content tokens (String literals,
/// Directive names, Error messages) which keep an owned std::string.
struct StringLitData {
    std::string value;
};

/// Token value - identifier-like tokens carry a SymbolId interned at
/// lex time; string-content tokens carry owned text. See PRD
/// prd-parser-codegen-correctness.md Phase 5.
using TokenValue = std::variant<std::monostate, NumericValue, SymbolId,
                                StringLitData, PitchValue>;

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

    /// Get interned SymbolId (assumes type == Identifier). Resolve to
    /// a view via `StringInterner::view(id)` when string text is
    /// needed (e.g. error messages).
    [[nodiscard]] SymbolId as_identifier() const {
        return std::get<SymbolId>(value);
    }

    /// Get owned string literal content (assumes type == String,
    /// Directive, or Error — anything carrying raw text that isn't an
    /// identifier symbol).
    [[nodiscard]] const std::string& as_string_lit() const {
        return std::get<StringLitData>(value).value;
    }

    /// Get pitch MIDI note (assumes type == PitchLit)
    [[nodiscard]] std::uint8_t as_pitch() const {
        return std::get<PitchValue>(value).midi_note;
    }
};

} // namespace akkado
