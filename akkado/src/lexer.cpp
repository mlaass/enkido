#include "akkado/lexer.hpp"
#include "akkado/music_theory.hpp"
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace akkado {

// Phase 5 (PRD-8): classifiers + cursor come from lex_primitives.
using namespace lex_primitives;

namespace {

// Keyword lookup table
const std::unordered_map<std::string_view, TokenType> keywords = {
    {"true",     TokenType::True},
    {"false",    TokenType::False},
    {"match",    TokenType::Match},
    {"fn",       TokenType::Fn},
    {"as",       TokenType::As},
    {"const",    TokenType::Const},
    {"import",   TokenType::Import},
    // Parameter type annotations are NOT keywords. They are uppercase
    // PascalCase identifiers (`Signal`, `Number`, `Pattern`, `Record`,
    // `Array`, `String`, `Function`, `Stream`) resolved contextually in
    // parse_optional_annotation by lexeme — so they stay usable as ordinary
    // identifiers elsewhere. See PRD prd-parameter-type-annotations.
};

} // namespace

Lexer::Lexer(std::string_view source, StringInterner& interner,
             std::string_view filename)
    : lex_primitives::CursorBase(source)
    , interner_(&interner)
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
        .value = StringLitData{std::string(message)}
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
        if (c == '_' && !is_alpha_numeric(peek())) {
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
        // Directives
        case '$': return lex_directive();

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
        case '#': return make_token(TokenType::Hash);
        case '%': return make_token(TokenType::Hole);
        case '@': return make_token(TokenType::At);
        case '~': return make_token(TokenType::Tilde);
        case '^': return make_token(TokenType::Caret);
        case '.':
            // Check for .. or ... tokens
            if (peek() == '.') {
                if (peek_next() == '.') {
                    advance();  // consume second '.'
                    advance();  // consume third '.'
                    return make_token(TokenType::DotDotDot);
                }
                advance();  // consume second '.'
                return make_token(TokenType::DotDot);
            }
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
            if (match('|')) {
                return make_token(TokenType::OrOr);
            }
            // Single | could be bitwise OR (future) or error
            return make_error_token("Expected '>' or '|' after '|'");

        case '&':
            if (match('&')) {
                return make_token(TokenType::AndAnd);
            }
            // Single & could be bitwise AND (future) or error
            return make_error_token("Expected '&' after '&'");

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
            if (match('>')) {
                return make_token(TokenType::Diamond);  // <> bus-routing terminator
            }
            if (match('=')) {
                return make_token(TokenType::LessEqual);
            }
            return make_token(TokenType::Less);

        case '>':
            if (match('>')) {
                return make_token(TokenType::Pipe);  // >> alias for |>
            }
            if (match('=')) {
                return make_token(TokenType::GreaterEqual);
            }
            return make_token(TokenType::Greater);

        // Strings (and pitch literals for single quotes)
        case '"':
        case '`':
            return lex_string(c);
        case '\'':
            // Try to lex as a pitch literal first ('c4', 'f#3', etc.)
            if (auto pitch = try_lex_pitch()) {
                return *pitch;
            }
            return lex_string(c);

        default:
            return make_error_token("Unexpected character");
    }
}

Token Lexer::lex_number() {
    // The first char (digit / '.' / '-') was already consumed by
    // lex_token's dispatch; scan_number continues from the cursor and
    // parses the full text from start_.
    auto res = scan_number(*this, start_, {
        .allow_exponent = true,
        .seen_dot = source_[start_] == '.',
    });
    if (!res.ok) {
        return make_error_token("Invalid number");
    }
    return make_token(TokenType::Number, NumericValue{res.value, res.is_integer});
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

    return make_token(TokenType::String, StringLitData{std::move(value)});
}

Token Lexer::lex_identifier() {
    // Check for timeline string prefix: t"..." or t`...`
    if (source_[start_] == 't' && current_ == start_ + 1) {
        char next = peek();
        if (next == '"' || next == '`') {
            return make_token(TokenType::Timeline);
        }
    }

    // PRD prd-patterns-as-scalar-values §5.1: typed pattern prefixes.
    // v"…" / n"…" / s"…" / c"…" — only when the letter is exactly one
    // character at this position and the next char is a quote/backtick.
    if (current_ == start_ + 1) {
        char next = peek();
        if (next == '"' || next == '`') {
            switch (source_[start_]) {
                case 'v': return make_token(TokenType::ValuePat);
                case 'n': return make_token(TokenType::NotePat);
                case 's': return make_token(TokenType::SamplePat);
                case 'c': return make_token(TokenType::ChordPat);
                default: break;
            }
        }
    }

    while (is_alpha_numeric(peek())) {
        advance();
    }

    std::string_view text = source_.substr(start_, current_ - start_);
    TokenType type = identifier_type(text);

    if (type == TokenType::Identifier) {
        // PRD prd-parser-codegen-correctness.md Phase 5 (F12): intern at
        // lex time. `text` views the source buffer; source must outlive
        // interner (compile()'s combined_source does).
        return make_token(type, interner_->intern(text));
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

Token Lexer::lex_directive() {
    // We've already consumed '$', now read the directive name
    if (!is_alpha(peek())) {
        return make_error_token("Expected directive name after '$'");
    }

    // Read identifier characters
    while (is_alpha_numeric(peek())) {
        advance();
    }

    // Extract directive name (skip the '$')
    std::string_view text = source_.substr(start_ + 1, current_ - start_ - 1);
    return make_token(TokenType::Directive, StringLitData{std::string(text)});
}

std::optional<Token> Lexer::try_lex_pitch() {
    // Try to match pitch pattern: [a-gA-G][#b]?[0-9]'
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

    // Check for closing quote
    if (lookahead >= source_.size() || source_[lookahead] != '\'') {
        return std::nullopt;
    }

    // We have a valid pitch literal - now parse it
    int octave = source_[octave_start] - '0';
    if (octave_end - octave_start > 1) {
        // Double digit octave
        octave = octave * 10 + (source_[octave_start + 1] - '0');
    }
    std::uint8_t midi_note = pitch_to_midi(note_char, accidental, octave);

    // Consume all characters including closing quote
    while (current_ < lookahead) {
        advance();
    }
    advance(); // closing quote

    return make_token(TokenType::PitchLit, PitchValue{midi_note});
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
lex(std::string_view source, StringInterner& interner,
    std::string_view filename) {
    Lexer lexer(source, interner, filename);
    auto tokens = lexer.lex_all();
    return {std::move(tokens), lexer.diagnostics()};
}

} // namespace akkado
