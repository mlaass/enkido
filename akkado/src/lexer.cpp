#include "akkado/lexer.hpp"
#include "akkado/music_theory.hpp"
#include <charconv>
#include <unordered_map>

namespace akkado {

namespace {

// Keyword lookup table
const std::unordered_map<std::string_view, TokenType> keywords = {
    {"true",     TokenType::True},
    {"false",    TokenType::False},
    {"post",     TokenType::Post},
    {"match",    TokenType::Match},
    {"fn",       TokenType::Fn},
    {"pat",      TokenType::Pat},
    {"seq",      TokenType::Seq},
    {"timeline", TokenType::Timeline},
    {"note",     TokenType::Note},
};

} // namespace

Lexer::Lexer(std::string_view source, std::string_view filename)
    : source_(source)
    , filename_(filename)
{}

std::vector<Token> Lexer::lex_all() {
    std::vector<Token> tokens;
    tokens.reserve(source_.size() / 4); // Rough estimate

    while (true) {
        Token tok = lex_token();
        tokens.push_back(tok);
        if (tok.type == TokenType::Eof) {
            break;
        }
    }

    return tokens;
}

bool Lexer::has_errors() const {
    return akkado::has_errors(diagnostics_);
}

bool Lexer::is_at_end() const {
    return current_ >= source_.size();
}

char Lexer::peek() const {
    if (is_at_end()) return '\0';
    return source_[current_];
}

char Lexer::peek_next() const {
    if (current_ + 1 >= source_.size()) return '\0';
    return source_[current_ + 1];
}

char Lexer::advance() {
    char c = source_[current_++];
    update_location(c);
    return c;
}

bool Lexer::match(char expected) {
    if (is_at_end()) return false;
    if (source_[current_] != expected) return false;
    advance();
    return true;
}

bool Lexer::is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool Lexer::is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

bool Lexer::is_alphanumeric(char c) {
    return is_alpha(c) || is_digit(c);
}

bool Lexer::is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

Token Lexer::make_token(TokenType type) {
    return Token{
        .type = type,
        .location = {
            .line = token_line_,
            .column = token_column_,
            .offset = start_,
            .length = current_ - start_
        },
        .lexeme = source_.substr(start_, current_ - start_),
        .value = {}
    };
}

Token Lexer::make_token(TokenType type, TokenValue value) {
    Token tok = make_token(type);
    tok.value = std::move(value);
    return tok;
}

Token Lexer::make_error_token(std::string_view message) {
    add_error(message);
    return Token{
        .type = TokenType::Error,
        .location = {
            .line = token_line_,
            .column = token_column_,
            .offset = start_,
            .length = current_ - start_
        },
        .lexeme = source_.substr(start_, current_ - start_),
        .value = std::string(message)
    };
}

void Lexer::skip_whitespace() {
    while (!is_at_end()) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                advance();
                break;
            case '/':
                if (peek_next() == '/') {
                    skip_line_comment();
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

void Lexer::skip_line_comment() {
    // Skip the //
    advance();
    advance();

    // Skip until end of line
    while (!is_at_end() && peek() != '\n') {
        advance();
    }
}

Token Lexer::lex_token() {
    skip_whitespace();

    start_ = current_;
    token_line_ = line_;
    token_column_ = column_;

    if (is_at_end()) {
        return make_token(TokenType::Eof);
    }

    char c = advance();

    // Identifiers and keywords (handle standalone _ specially)
    if (is_alpha(c)) {
        if (c == '_' && !is_alphanumeric(peek())) {
            return make_token(TokenType::Underscore);
        }
        return lex_identifier();
    }

    // Numbers
    if (is_digit(c)) {
        return lex_number();
    }

    // Single and multi-character tokens
    switch (c) {
        // Single character tokens
        case '(': return make_token(TokenType::LParen);
        case ')': return make_token(TokenType::RParen);
        case '[': return make_token(TokenType::LBracket);
        case ']': return make_token(TokenType::RBracket);
        case '{': return make_token(TokenType::LBrace);
        case '}': return make_token(TokenType::RBrace);
        case ',': return make_token(TokenType::Comma);
        case ':': return make_token(TokenType::Colon);
        case ';': return make_token(TokenType::Semicolon);
        case '%': return make_token(TokenType::Hole);
        case '@': return make_token(TokenType::At);
        case '~': return make_token(TokenType::Tilde);
        case '^': return make_token(TokenType::Caret);
        case '.':
            // Check for leading decimal number (.001, .5)
            if (is_digit(peek())) {
                return lex_number();
            }
            return make_token(TokenType::Dot);

        // Potentially multi-character tokens
        case '+': return make_token(TokenType::Plus);
        case '*': return make_token(TokenType::Star);
        case '/': return make_token(TokenType::Slash);

        case '-':
            if (match('>')) {
                return make_token(TokenType::Arrow);
            }
            // Check if this is a negative number
            if (is_digit(peek())) {
                return lex_number();
            }
            return make_token(TokenType::Minus);

        case '|':
            if (match('>')) {
                return make_token(TokenType::Pipe);
            }
            // Single | could be used in mini-notation for choice
            return make_error_token("Expected '>' after '|' for pipe operator");

        case '=':
            if (match('=')) {
                return make_token(TokenType::EqualEqual);
            }
            return make_token(TokenType::Equals);

        case '!':
            if (match('=')) {
                return make_token(TokenType::BangEqual);
            }
            return make_token(TokenType::Bang);

        case '?':
            return make_token(TokenType::Question);

        case '<':
            if (match('=')) {
                return make_token(TokenType::LessEqual);
            }
            return make_token(TokenType::Less);

        case '>':
            if (match('=')) {
                return make_token(TokenType::GreaterEqual);
            }
            return make_token(TokenType::Greater);

        // Strings (and pitch literals for single quotes)
        case '"':
        case '`':
            return lex_string(c);
        case '\'':
            // Try to lex as pitch or chord literal first ('c4', 'f#3', 'c4:maj', etc.)
            if (auto pitch_or_chord = try_lex_pitch_or_chord()) {
                return *pitch_or_chord;
            }
            return lex_string(c);

        default:
            return make_error_token("Unexpected character");
    }
}

Token Lexer::lex_number() {
    bool has_dot = false;
    bool has_exponent = false;
    bool is_negative = source_[start_] == '-';

    // Handle leading decimal (.001, .5)
    if (source_[start_] == '.') {
        has_dot = true;
        // Digits after decimal already confirmed by caller
        while (is_digit(peek())) {
            advance();
        }
    } else {
        // Consume digits before decimal point
        while (is_digit(peek())) {
            advance();
        }

        // Look for decimal part
        if (peek() == '.' && is_digit(peek_next())) {
            has_dot = true;
            advance(); // consume '.'

            while (is_digit(peek())) {
                advance();
            }
        }
    }

    // Look for scientific notation (e/E with optional +/- and digits)
    if (peek() == 'e' || peek() == 'E') {
        char next = peek_next();
        bool valid_exponent = is_digit(next) ||
            ((next == '+' || next == '-') && current_ + 2 < source_.size() &&
             is_digit(source_[current_ + 2]));

        if (valid_exponent) {
            has_exponent = true;
            advance(); // consume 'e' or 'E'

            if (peek() == '+' || peek() == '-') {
                advance(); // consume sign
            }

            if (!is_digit(peek())) {
                return make_error_token("Expected digit after exponent");
            }

            while (is_digit(peek())) {
                advance();
            }
        }
    }

    // Parse the number
    std::string_view text = source_.substr(start_, current_ - start_);
    double value = 0.0;

    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return make_error_token("Invalid number");
    }

    // Integer only if no decimal and no exponent
    bool is_integer = !has_dot && !has_exponent;
    return make_token(TokenType::Number, NumericValue{value, is_integer});
}

Token Lexer::lex_string(char quote) {
    std::string value;
    value.reserve(32);

    while (!is_at_end() && peek() != quote) {
        char c = peek();

        if (c == '\n') {
            // Allow multi-line strings for mini-notation
            value += c;
            advance();
            continue;
        }

        if (c == '\\') {
            advance(); // consume backslash
            if (is_at_end()) {
                return make_error_token("Unterminated string escape");
            }

            char escaped = advance();
            switch (escaped) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '`':  value += '`';  break;
                default:
                    return make_error_token("Invalid escape sequence");
            }
        } else {
            value += advance();
        }
    }

    if (is_at_end()) {
        return make_error_token("Unterminated string");
    }

    advance(); // consume closing quote

    return make_token(TokenType::String, std::move(value));
}

Token Lexer::lex_identifier() {
    while (is_alphanumeric(peek())) {
        advance();
    }

    std::string_view text = source_.substr(start_, current_ - start_);
    TokenType type = identifier_type(text);

    if (type == TokenType::Identifier) {
        return make_token(type, std::string(text));
    }

    return make_token(type);
}

TokenType Lexer::identifier_type(std::string_view text) const {
    auto it = keywords.find(text);
    if (it != keywords.end()) {
        return it->second;
    }
    return TokenType::Identifier;
}

std::optional<Token> Lexer::try_lex_pitch_or_chord() {
    // Try to match pitch pattern: [a-gA-G][#b]?[0-9]'
    // Or chord pattern: [a-gA-G][#b]?[0-9]:[chord_type]'
    // We're positioned right after the opening quote
    std::size_t lookahead = current_;

    // Check for note letter
    if (lookahead >= source_.size()) return std::nullopt;
    char note_char = source_[lookahead];
    if (!((note_char >= 'a' && note_char <= 'g') ||
          (note_char >= 'A' && note_char <= 'G'))) {
        return std::nullopt;
    }
    lookahead++;

    // Check for optional accidental (# or b)
    int accidental = 0;
    if (lookahead < source_.size()) {
        char acc_char = source_[lookahead];
        if (acc_char == '#') {
            accidental = 1;
            lookahead++;
        } else if (acc_char == 'b') {
            accidental = -1;
            lookahead++;
        }
    }

    // Check for octave digit
    if (lookahead >= source_.size()) return std::nullopt;
    if (!is_digit(source_[lookahead])) return std::nullopt;
    std::size_t octave_start = lookahead;
    lookahead++;

    // Allow double-digit octave (e.g., 10)
    if (lookahead < source_.size() && is_digit(source_[lookahead])) {
        lookahead++;
    }
    std::size_t octave_end = lookahead;

    // Check for chord type (colon followed by chord name)
    bool is_chord = false;
    const std::vector<std::int8_t>* chord_intervals = nullptr;

    if (lookahead < source_.size() && source_[lookahead] == ':') {
        // Could be a chord - look for chord type
        std::size_t chord_start = lookahead + 1;
        std::size_t chord_end = chord_start;

        // Scan for chord type name (alphanumeric)
        while (chord_end < source_.size() &&
               (is_alpha(source_[chord_end]) || is_digit(source_[chord_end]))) {
            chord_end++;
        }

        if (chord_end > chord_start) {
            std::string_view chord_name = source_.substr(chord_start, chord_end - chord_start);
            chord_intervals = lookup_chord(chord_name);

            if (chord_intervals != nullptr) {
                // Valid chord type found
                is_chord = true;
                lookahead = chord_end;
            }
        }
    }

    // Check for closing quote
    if (lookahead >= source_.size() || source_[lookahead] != '\'') {
        return std::nullopt;
    }

    // We have a valid pitch or chord literal - now parse it
    // Note letter semitones: C=0, D=2, E=4, F=5, G=7, A=9, B=11
    static constexpr int semitones[] = {9, 11, 0, 2, 4, 5, 7}; // a, b, c, d, e, f, g
    char note_lower = note_char | 0x20; // to lowercase
    int note_semitone = semitones[note_lower - 'a'];

    // Parse octave
    int octave = source_[octave_start] - '0';
    if (octave_end - octave_start > 1) {
        // Double digit octave
        octave = octave * 10 + (source_[octave_start + 1] - '0');
    }

    // MIDI note: (octave + 1) * 12 + semitone + accidental
    int midi_note = (octave + 1) * 12 + note_semitone + accidental;

    // Clamp to valid MIDI range
    if (midi_note < 0) midi_note = 0;
    if (midi_note > 127) midi_note = 127;

    // Consume all characters including closing quote
    while (current_ < lookahead) {
        advance();
    }
    advance(); // closing quote

    if (is_chord) {
        return make_token(TokenType::ChordLit, ChordValue{
            static_cast<std::uint8_t>(midi_note),
            *chord_intervals
        });
    } else {
        return make_token(TokenType::PitchLit, PitchValue{static_cast<std::uint8_t>(midi_note)});
    }
}

void Lexer::add_error(std::string_view message) {
    add_error(message, current_location());
}

void Lexer::add_error(std::string_view message, SourceLocation loc) {
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .code = "L001",
        .message = std::string(message),
        .filename = filename_,
        .location = loc
    });
}

void Lexer::update_location(char c) {
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
}

SourceLocation Lexer::current_location() const {
    return {
        .line = token_line_,
        .column = token_column_,
        .offset = start_,
        .length = current_ - start_
    };
}

// Convenience function
std::pair<std::vector<Token>, std::vector<Diagnostic>>
lex(std::string_view source, std::string_view filename) {
    Lexer lexer(source, filename);
    auto tokens = lexer.lex_all();
    return {std::move(tokens), lexer.diagnostics()};
}

} // namespace akkado
