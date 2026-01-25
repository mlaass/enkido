#include "akkado/mini_lexer.hpp"
#include "akkado/music_theory.hpp"
#include <charconv>

namespace akkado {

MiniLexer::MiniLexer(std::string_view pattern, SourceLocation base_location)
    : pattern_(pattern)
    , base_location_(base_location)
{}

std::vector<MiniToken> MiniLexer::lex_all() {
    std::vector<MiniToken> tokens;
    tokens.reserve(pattern_.size() / 2);

    while (true) {
        MiniToken tok = lex_token();
        tokens.push_back(tok);
        if (tok.type == MiniTokenType::Eof) {
            break;
        }
    }

    return tokens;
}

bool MiniLexer::has_errors() const {
    return akkado::has_errors(diagnostics_);
}

bool MiniLexer::is_at_end() const {
    return current_ >= pattern_.size();
}

char MiniLexer::peek() const {
    if (is_at_end()) return '\0';
    return pattern_[current_];
}

char MiniLexer::peek_next() const {
    if (current_ + 1 >= pattern_.size()) return '\0';
    return pattern_[current_ + 1];
}

char MiniLexer::peek_ahead(std::size_t n) const {
    if (current_ + n >= pattern_.size()) return '\0';
    return pattern_[current_ + n];
}

char MiniLexer::advance() {
    char c = pattern_[current_++];
    column_++;
    return c;
}

bool MiniLexer::match(char expected) {
    if (is_at_end()) return false;
    if (pattern_[current_] != expected) return false;
    advance();
    return true;
}

bool MiniLexer::is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool MiniLexer::is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

bool MiniLexer::is_pitch_letter(char c) {
    return (c >= 'a' && c <= 'g') || (c >= 'A' && c <= 'G');
}

bool MiniLexer::is_accidental(char c) {
    return c == '#' || c == 'b';
}

bool MiniLexer::is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

MiniToken MiniLexer::make_token(MiniTokenType type) {
    return MiniToken{
        .type = type,
        .location = current_location(),
        .lexeme = pattern_.substr(start_, current_ - start_),
        .value = {}
    };
}

MiniToken MiniLexer::make_token(MiniTokenType type, MiniTokenValue value) {
    MiniToken tok = make_token(type);
    tok.value = std::move(value);
    return tok;
}

MiniToken MiniLexer::make_error_token(std::string_view message) {
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .code = "M001",
        .message = std::string(message),
        .filename = "<pattern>",
        .location = current_location()
    });

    return MiniToken{
        .type = MiniTokenType::Error,
        .location = current_location(),
        .lexeme = pattern_.substr(start_, current_ - start_),
        .value = std::string(message)
    };
}

void MiniLexer::skip_whitespace() {
    while (!is_at_end() && is_whitespace(peek())) {
        advance();
    }
}

SourceLocation MiniLexer::current_location() const {
    return {
        .line = base_location_.line,
        .column = base_location_.column + start_,
        .offset = base_location_.offset + start_,
        .length = current_ - start_
    };
}

bool MiniLexer::looks_like_pitch() const {
    // Check if current position looks like a pitch: [a-gA-G][#b]?[0-9]?
    // followed by whitespace, modifier, or end
    if (!is_pitch_letter(peek())) return false;

    std::size_t pos = current_ + 1;

    // Optional accidental
    if (pos < pattern_.size() && is_accidental(pattern_[pos])) {
        pos++;
    }

    // Optional octave digit(s)
    while (pos < pattern_.size() && is_digit(pattern_[pos])) {
        pos++;
    }

    // Must be followed by: end, whitespace, modifier, bracket, angle, paren, brace, comma, pipe, colon (for chord), percent
    if (pos >= pattern_.size()) return true;

    char next = pattern_[pos];
    return is_whitespace(next) ||
           next == '*' || next == '/' || next == '@' || next == '!' || next == '?' || next == '%' ||
           next == '[' || next == ']' || next == '<' || next == '>' ||
           next == '(' || next == ')' || next == '{' || next == '}' ||
           next == ',' || next == '|' || next == ':';
}

std::uint8_t MiniLexer::parse_pitch_to_midi(char note, int accidental, int octave) const {
    // Note letter semitones: A=9, B=11, C=0, D=2, E=4, F=5, G=7
    static constexpr int semitones[] = {9, 11, 0, 2, 4, 5, 7}; // a, b, c, d, e, f, g
    char note_lower = note | 0x20;
    int note_semitone = semitones[note_lower - 'a'];

    // MIDI note: (octave + 1) * 12 + semitone + accidental
    int midi_note = (octave + 1) * 12 + note_semitone + accidental;

    // Clamp to valid MIDI range
    if (midi_note < 0) midi_note = 0;
    if (midi_note > 127) midi_note = 127;

    return static_cast<std::uint8_t>(midi_note);
}

MiniToken MiniLexer::lex_token() {
    skip_whitespace();

    start_ = current_;

    if (is_at_end()) {
        return make_token(MiniTokenType::Eof);
    }

    char c = peek();

    // Handle _ as rest (before alpha check since _ is in is_alpha)
    if (c == '_') {
        advance();
        return make_token(MiniTokenType::Rest);
    }

    // Check for pitch token first (highest priority for a-g)
    if (looks_like_pitch()) {
        return lex_pitch_or_sample();
    }

    // Sample/identifier tokens (other letters, excluding _)
    if (is_alpha(c)) {
        return lex_pitch_or_sample();
    }

    // Numbers (for modifiers and euclidean)
    if (is_digit(c) || (c == '.' && is_digit(peek_next()))) {
        return lex_number();
    }

    // Advance for single-character tokens
    advance();

    switch (c) {
        // Rests (note: '_' is handled above before is_alpha check)
        case '~': return make_token(MiniTokenType::Rest);

        // Groupings
        case '[': return make_token(MiniTokenType::LBracket);
        case ']': return make_token(MiniTokenType::RBracket);
        case '<': return make_token(MiniTokenType::LAngle);
        case '>': return make_token(MiniTokenType::RAngle);
        case '(': return make_token(MiniTokenType::LParen);
        case ')': return make_token(MiniTokenType::RParen);
        case '{': return make_token(MiniTokenType::LBrace);
        case '}': return make_token(MiniTokenType::RBrace);
        case ',': return make_token(MiniTokenType::Comma);

        // Modifiers
        case '*': return make_token(MiniTokenType::Star);
        case '/': return make_token(MiniTokenType::Slash);
        case ':': return make_token(MiniTokenType::Colon);
        case '@': return make_token(MiniTokenType::At);
        case '!': return make_token(MiniTokenType::Bang);
        case '?': return make_token(MiniTokenType::Question);
        case '%': return make_token(MiniTokenType::Percent);

        // Choice
        case '|': return make_token(MiniTokenType::Pipe);

        default:
            return make_error_token("Unexpected character in pattern");
    }
}

MiniToken MiniLexer::lex_number() {
    bool has_dot = false;

    // Handle leading decimal
    if (peek() == '.') {
        has_dot = true;
        advance();
    }

    // Consume integer part
    while (is_digit(peek())) {
        advance();
    }

    // Look for decimal part
    if (!has_dot && peek() == '.' && is_digit(peek_next())) {
        has_dot = true;
        advance(); // consume '.'

        while (is_digit(peek())) {
            advance();
        }
    }

    // Parse the number
    std::string_view text = pattern_.substr(start_, current_ - start_);
    double value = 0.0;

    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return make_error_token("Invalid number in pattern");
    }

    return make_token(MiniTokenType::Number, value);
}

MiniToken MiniLexer::lex_pitch_or_sample() {
    // Consume identifier-like characters
    while (!is_at_end()) {
        char c = peek();
        if (is_alpha(c) || is_digit(c) || c == '#') {
            advance();
        } else {
            break;
        }
    }

    std::string_view text = pattern_.substr(start_, current_ - start_);

    // Try to parse as pitch: [a-gA-G][#b]?[0-9]*
    if (text.size() >= 1 && is_pitch_letter(text[0])) {
        std::size_t pos = 1;
        int accidental = 0;
        int octave = 4; // Default octave for mini-notation
        bool has_octave = false;

        // Check for accidental
        if (pos < text.size()) {
            if (text[pos] == '#') {
                accidental = 1;
                pos++;
            } else if (text[pos] == 'b' && (pos + 1 >= text.size() || !is_alpha(text[pos + 1]))) {
                // 'b' is flat only if not followed by more letters (to distinguish from samples like "bd")
                accidental = -1;
                pos++;
            }
        }

        // Check for octave
        if (pos < text.size() && is_digit(text[pos])) {
            has_octave = true;
            octave = text[pos] - '0';
            pos++;

            // Double-digit octave
            if (pos < text.size() && is_digit(text[pos])) {
                octave = octave * 10 + (text[pos] - '0');
                pos++;
            }
        }

        // If we consumed everything, it's a pitch
        if (pos == text.size()) {
            std::uint8_t midi = parse_pitch_to_midi(text[0], accidental, octave);
            return make_token(MiniTokenType::PitchToken, MiniPitchData{midi, has_octave});
        }
    }

    // Not a pitch - treat as sample token
    // Check for variant suffix (e.g., :2)
    std::uint8_t variant = 0;
    if (peek() == ':' && is_digit(peek_next())) {
        advance(); // consume ':'
        std::size_t var_start = current_;
        while (is_digit(peek())) {
            advance();
        }
        std::string_view var_text = pattern_.substr(var_start, current_ - var_start);
        int var_val = 0;
        std::from_chars(var_text.data(), var_text.data() + var_text.size(), var_val);
        variant = static_cast<std::uint8_t>(var_val);
    }

    return make_token(MiniTokenType::SampleToken, MiniSampleData{std::string(text), variant});
}

// Convenience function
std::pair<std::vector<MiniToken>, std::vector<Diagnostic>>
lex_mini(std::string_view pattern, SourceLocation base_location) {
    MiniLexer lexer(pattern, base_location);
    auto tokens = lexer.lex_all();
    return {std::move(tokens), lexer.diagnostics()};
}

} // namespace akkado
