#include "akkado/mini_lexer.hpp"
#include "akkado/chord_parser.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace akkado {

static MiniParseMode mode_from_bools(bool sample_only, bool curve_mode) {
    if (curve_mode) return MiniParseMode::Curve;
    if (sample_only) return MiniParseMode::Sample;
    return MiniParseMode::Note;
}

MiniLexer::MiniLexer(std::string_view pattern, SourceLocation base_location, MiniParseMode mode)
    : pattern_(pattern)
    , base_location_(base_location)
    , mode_(mode)
    // Sample mode disables pitch/chord detection entirely (every alpha atom
    // is a sample). Chord mode goes through the standard lex path so that
    // try_lex_chord_symbol() recognises Am / C7 / Fmaj7 as ChordToken atoms.
    , sample_only_(mode == MiniParseMode::Sample)
    , curve_mode_(mode == MiniParseMode::Curve)
    , value_mode_(mode == MiniParseMode::Value)
    , note_mode_(mode == MiniParseMode::Note)
{}

MiniLexer::MiniLexer(std::string_view pattern, SourceLocation base_location, bool sample_only, bool curve_mode)
    : MiniLexer(pattern, base_location, mode_from_bools(sample_only, curve_mode))
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
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
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
    // Line 1 of the pattern keeps the historical column offset (base.column +
    // start_) so single-line callers see byte-identical output. Line 2+ uses
    // the pattern-relative column — we don't know the source-file indentation
    // of continuation lines, so we report start_column_ as-is.
    SourceLocation loc;
    loc.line = base_location_.line + (start_line_ - 1);
    loc.column = (start_line_ == 1)
        ? (base_location_.column + (start_column_ - 1))
        : start_column_;
    loc.offset = base_location_.offset + start_;
    loc.length = current_ - start_;
    return loc;
}

bool MiniLexer::looks_like_pitch() const {
    // Check if current position looks like a pitch: [a-gA-G][#bx^v+]*[0-9]?
    // followed by whitespace, modifier, or end
    //
    // IMPORTANT: Uppercase letters without octave (A, C, G) or with chord quality (Am, C7)
    // should NOT be detected as pitches - they will be handled as chords.
    // Only lowercase (c4, a3) or uppercase WITH explicit octave (A4, C5) are pitches.
    if (!is_pitch_letter(peek())) return false;

    char first = peek();
    bool is_uppercase = (first >= 'A' && first <= 'G');

    std::size_t pos = current_ + 1;

    // Scan modifier stream: accidentals (#, b, x) and micro-step operators
    // (^, v, +, d, \). `d` is ambiguous with note D and with sample names
    // like "bd", "sd". Per PRD §4.2 it acts as an alias only when the token
    // unambiguously continues into an octave digit — otherwise we leave it
    // for the sample lexer. `\` is unambiguous: never appears in sample names.
    auto chain_reaches_digit = [this](std::size_t scan) {
        while (scan < pattern_.size()) {
            char sc = pattern_[scan];
            if (is_digit(sc)) return true;
            if (sc == '#' || sc == 'b' || sc == 'x' ||
                sc == '^' || sc == 'v' || sc == '+' ||
                sc == 'd' || sc == '\\') {
                scan++;
            } else {
                return false;
            }
        }
        return false;
    };
    while (pos < pattern_.size()) {
        char c = pattern_[pos];
        if (c == '#' || c == 'b' || c == 'x' ||
            c == '^' || c == 'v' || c == '+' || c == '\\') {
            pos++;
        } else if (c == 'd' && chain_reaches_digit(pos + 1)) {
            pos++;
        } else {
            break;
        }
    }

    // For uppercase letters, REQUIRE an octave digit to be a pitch
    // This allows "A", "Am", "C7", "G" to fall through to chord detection
    // while "A4", "C5" are still recognized as pitches
    if (is_uppercase) {
        // Must have at least one digit for octave
        if (pos >= pattern_.size() || !is_digit(pattern_[pos])) {
            return false;  // No octave -> not a pitch (could be chord)
        }
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
    start_line_ = line_;
    start_column_ = column_;

    if (is_at_end()) {
        return make_token(MiniTokenType::Eof);
    }

    // Snapshot then clear: any code path below that emits a modifier token
    // sets `last_was_modifier_` back to true before returning.
    // Force numeric lexing while inside euclidean `( … )` argument list.
    const bool prev_was_modifier = last_was_modifier_ || paren_depth_ > 0;
    last_was_modifier_ = false;

    char c = peek();

    // Curve-mode handling: reinterpret certain characters as curve tokens
    if (curve_mode_) {
        switch (c) {
            case '_':
                advance();
                return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.00f});
            case '.':
                // '.' followed by a digit is a number (for modifiers like @0.5)
                if (!is_digit(peek_next())) {
                    advance();
                    return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.25f});
                }
                break;  // fall through to number lexing
            case '-':
                advance();
                return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.50f});
            case '^':
                advance();
                return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{0.75f});
            case '\'':
                advance();
                return make_token(MiniTokenType::CurveLevel, MiniCurveLevelData{1.00f});
            case '~':
                advance();
                return make_token(MiniTokenType::CurveSmooth);
            case '\\':
                advance();
                return make_token(MiniTokenType::CurveRamp);
            case '/':
                // '/' followed by a digit is Slash (slow modifier); otherwise CurveRamp
                if (is_digit(peek_next())) {
                    advance();
                    last_was_modifier_ = true;
                    return make_token(MiniTokenType::Slash);
                }
                advance();
                return make_token(MiniTokenType::CurveRamp);
            default:
                break;  // fall through to standard lexing
        }
    }

    // Handle _ as elongate (extends previous note - Tidal-compatible)
    if (c == '_') {
        advance();
        return make_token(MiniTokenType::Elongate);
    }

    // Value mode (v"…"): atoms must be numeric literals.
    // Negative numbers, decimals, scientific notation are accepted.
    // Reject any letter-leading token with E163.
    // Skip when the previous token was a modifier: a number following `@`/`*`/`/`/`!`/`?`
    // is the modifier's argument, not a value-atom, and must be a plain Number token.
    if (value_mode_ && !prev_was_modifier) {
        // Possible numeric atom: optional sign + digits (or .digits).
        bool starts_numeric = is_digit(c) || (c == '.' && is_digit(peek_next()));
        if (c == '-' || c == '+') {
            // Distinguish leading sign on a numeric atom from a stray operator.
            char nx = peek_next();
            if (is_digit(nx) || (nx == '.' && current_ + 2 < pattern_.size() && is_digit(pattern_[current_ + 2]))) {
                starts_numeric = true;
            }
        }
        if (starts_numeric) {
            return lex_value_atom();
        }
        // Letter-leading atoms are an error in v"…" mode.
        if (is_alpha(c)) {
            // Consume the bad word for a focused error span.
            while (!is_at_end() && (is_alpha(peek()) || is_digit(peek()) || peek() == '#')) {
                advance();
            }
            std::string_view bad = pattern_.substr(start_, current_ - start_);
            std::string msg = "atom '";
            msg += bad;
            msg += "' is not a numeric literal in v\"…\" mode (E163)";
            return make_error_token(msg);
        }
        // Fall through to standard punctuation lexing for [, <, {, etc.
    }

    // Note mode (n"…"): accept raw MIDI numbers as pitch atoms in addition
    // to note names — per the token comment "note name + bare-MIDI pattern".
    // Letter-leading atoms still fall through to standard pitch detection so
    // `n"c4 e4 g4"` continues to work alongside `n"60 64 67"`.
    // Skip when the previous token was a modifier: digits after `@`/`*`/`/`/`!`/`?`
    // are the modifier's argument and must lex as a plain Number, not a PitchToken.
    if (note_mode_ && !prev_was_modifier) {
        bool starts_numeric = is_digit(c) || (c == '.' && is_digit(peek_next()));
        if (starts_numeric) {
            return lex_note_atom();
        }
    }

    // In sample_only mode (chord patterns), skip pitch detection
    if (!sample_only_) {
        // For uppercase A-G, try chord detection FIRST
        // This handles "Am", "C7", "Fmaj7", "G" as chords
        // Only "A4", "C5" (with explicit 2-digit octave-like numbers) should be pitches
        if (c >= 'A' && c <= 'G') {
            if (auto chord_tok = try_lex_chord_symbol()) {
                return *chord_tok;
            }
            // Chord detection failed, try as pitch
            if (looks_like_pitch()) {
                return lex_pitch();
            }
        }

        // For lowercase a-g, check if it looks like a pitch
        if (looks_like_pitch()) {
            return lex_pitch();
        }
    }

    // Sample/identifier tokens (other letters, excluding _)
    if (is_alpha(c)) {
        return lex_sample_only();
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
        case '(': ++paren_depth_; return make_token(MiniTokenType::LParen);
        case ')':
            if (paren_depth_ > 0) --paren_depth_;
            return make_token(MiniTokenType::RParen);
        case '{': return make_token(MiniTokenType::LBrace);
        case '}': return make_token(MiniTokenType::RBrace);
        case ',': return make_token(MiniTokenType::Comma);

        // Modifiers. The five modifiers that take a numeric argument set
        // last_was_modifier_ so the next token routes through lex_number()
        // even inside v"…" / n"…". `:` and `%` are not value-taking modifiers.
        case '*': last_was_modifier_ = true; return make_token(MiniTokenType::Star);
        case '/': last_was_modifier_ = true; return make_token(MiniTokenType::Slash);
        case ':': return make_token(MiniTokenType::Colon);
        case '@': last_was_modifier_ = true; return make_token(MiniTokenType::At);
        case '!': last_was_modifier_ = true; return make_token(MiniTokenType::Bang);
        case '?': last_was_modifier_ = true; return make_token(MiniTokenType::Question);
        // `%` is the polymeter step-count modifier (`{a b}%N`): N is always
        // a plain Number, so route the next token through lex_number() even
        // inside `n"…"` / `v"…"`.
        case '%': last_was_modifier_ = true; return make_token(MiniTokenType::Percent);

        // Choice
        case '|': return make_token(MiniTokenType::Pipe);

        default:
            return make_error_token("Unexpected character in pattern");
    }
}

MiniToken MiniLexer::lex_value_atom() {
    // Lex a numeric literal for v"…" mode atoms.
    // Accepts: optional leading +/-, integer part, optional fractional part,
    // optional exponent (e/E with optional sign).
    if (peek() == '+' || peek() == '-') {
        advance();
    }
    bool has_digit = false;
    while (is_digit(peek())) { advance(); has_digit = true; }
    if (peek() == '.') {
        advance();
        while (is_digit(peek())) { advance(); has_digit = true; }
    }
    // Exponent
    if (peek() == 'e' || peek() == 'E') {
        // Look ahead so we don't consume an 'e' that's part of an identifier.
        std::size_t save = current_;
        advance();  // consume e/E
        if (peek() == '+' || peek() == '-') advance();
        bool exp_has_digit = false;
        while (is_digit(peek())) { advance(); exp_has_digit = true; }
        if (!exp_has_digit) {
            // Roll back the 'e' — wasn't a valid exponent.
            current_ = save;
        }
    }
    if (!has_digit) {
        return make_error_token("expected numeric atom in v\"…\" mode (E163)");
    }
    std::string_view text = pattern_.substr(start_, current_ - start_);
    std::string buf(text);
    char* end = nullptr;
    double value = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str() || !std::isfinite(value)) {
        return make_error_token("invalid numeric atom in v\"…\" mode (E163)");
    }
    return make_token(MiniTokenType::ValueAtom, value);
}

MiniToken MiniLexer::lex_note_atom() {
    // Lex a numeric MIDI note for n"…" mode atoms. Mirrors lex_value_atom()
    // but emits a PitchToken so the rest of the mini-AST treats `n"60"` the
    // same as `n"c4"` (both become MIDI 60 pitch events with full extended
    // field wiring).
    bool has_digit = false;
    while (is_digit(peek())) { advance(); has_digit = true; }
    if (peek() == '.') {
        advance();
        while (is_digit(peek())) { advance(); has_digit = true; }
    }
    if (!has_digit) {
        return make_error_token("expected MIDI note number in n\"…\" mode");
    }
    std::string_view text = pattern_.substr(start_, current_ - start_);
    std::string buf(text);
    char* end = nullptr;
    double value = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str() || !std::isfinite(value) || value < 0.0 || value > 127.0) {
        return make_error_token("MIDI note out of range 0..127 in n\"…\" mode");
    }

    // Optional :velocity suffix matches lex_pitch().
    float velocity = 1.0f;
    if (peek() == ':' && (is_digit(peek_next()) || peek_next() == '.')) {
        advance();
        std::size_t vel_start = current_;
        if (peek() == '.') advance();
        while (is_digit(peek())) advance();
        if (pattern_[vel_start] != '.' && peek() == '.' && is_digit(peek_next())) {
            advance();
            while (is_digit(peek())) advance();
        }
        std::string_view vel_text = pattern_.substr(vel_start, current_ - vel_start);
        std::string vel_buf(vel_text);
        char* vel_end = nullptr;
        double vel_val = std::strtod(vel_buf.c_str(), &vel_end);
        if (vel_end == vel_buf.c_str()) vel_val = 0.0;
        velocity = static_cast<float>(std::clamp(vel_val, 0.0, 1.0));
    }

    auto props = try_lex_record_suffix();

    // Fractional MIDI notes round to the nearest semitone for the integer
    // midi_note slot; cents-level microtonal expressivity remains via the
    // ^v+\ modifiers on note-name pitches (e.g. `n"c4^"`).
    int integer_midi = static_cast<int>(std::round(value));
    if (integer_midi < 0) integer_midi = 0;
    if (integer_midi > 127) integer_midi = 127;
    return make_token(
        MiniTokenType::PitchToken,
        MiniPitchData{static_cast<std::uint8_t>(integer_midi),
                      true, velocity, /*micro_offset=*/0, std::move(props)});
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
    std::string buf(text);
    char* end = nullptr;
    double value = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str()) {
        return make_error_token("Invalid number in pattern");
    }

    return make_token(MiniTokenType::Number, value);
}

std::optional<MiniToken> MiniLexer::try_lex_chord_symbol() {
    // Chord symbols start with uppercase A-G
    // Examples: Am, C7, Fmaj7, Dm7, Bb, G#dim, Esus4
    //
    // Pattern: [A-G][#b]?<quality>
    // where quality is one of: "", "m", "min", "maj", "7", "maj7", "m7", "dim", "aug", etc.
    //
    // Chord symbols are distinguished from pitches by:
    // - Starting with uppercase letter
    // - Having a quality suffix (letters after optional accidental)
    // - NOT having an octave number
    //
    // Note: We cannot just use parse_chord_symbol because we need to:
    // 1. Look ahead without consuming
    // 2. Distinguish from pitches (e.g., "A4" is pitch, "Am" is chord)

    char first = peek();
    // Must start with uppercase A-G
    if (first < 'A' || first > 'G') {
        return std::nullopt;
    }

    // Scan ahead to see the whole token
    std::size_t scan_pos = current_ + 1;

    // Optional accidental
    if (scan_pos < pattern_.size() && (pattern_[scan_pos] == '#' || pattern_[scan_pos] == 'b')) {
        scan_pos++;
    }

    // Scan the rest of the token (quality part)
    // This can include letters and digits (for qualities like "m7", "maj7", "7", "9")
    while (scan_pos < pattern_.size()) {
        char c = pattern_[scan_pos];
        // Chord quality can contain letters, digits (for 7, 9, etc.), and some symbols
        if (is_alpha(c) || is_digit(c) || c == '^' || c == '-' || c == '+') {
            scan_pos++;
        } else {
            break;
        }
    }

    // Extract the potential chord symbol
    std::string_view chord_text = pattern_.substr(current_, scan_pos - current_);

    // Check if this could be a pitch with octave (e.g., "A4", "C#5", "Bb5")
    // The heuristic: if the "quality" is JUST a single digit, it's likely an octave
    // Exception: for chord symbols without accidentals, "5", "6", "7", "9" are valid chord qualities
    //
    // Examples:
    // - "A4" -> pitch (A in octave 4, since 4 is not a chord quality)
    // - "C7" -> chord (C dominant 7th)
    // - "Bb5" -> pitch (Bb in octave 5, because with accidental it looks like octave)
    // - "G5" -> could be either, but prefer pitch for consistency with "Bb5"
    std::size_t quality_start = current_ + 1;
    bool has_accidental = false;
    if (quality_start < pattern_.size() && pattern_[quality_start] == '#') {
        has_accidental = true;
        quality_start++;
    } else if (quality_start < pattern_.size() && pattern_[quality_start] == 'b') {
        has_accidental = true;
        quality_start++;
    }

    // If quality is just a single digit, decide based on context
    if (scan_pos == quality_start + 1 && is_digit(pattern_[quality_start])) {
        char digit = pattern_[quality_start];
        // With accidental (like Bb5, F#4), treat as pitch
        if (has_accidental) {
            return std::nullopt;  // Pitch with accidental and octave
        }
        // Without accidental: 5, 6, 7, 9 are valid chord qualities; others are octaves
        if (digit == '0' || digit == '1' || digit == '2' || digit == '3' || digit == '4' || digit == '8') {
            return std::nullopt;  // Likely a pitch octave
        }
        // 5, 6, 7, 9 -> could be chord quality, continue to chord parsing
    }

    // Empty quality means major chord - valid
    // But single uppercase letter without any suffix could be ambiguous
    // "C" alone should be treated as C major chord
    // "A" alone could be A4 pitch or A major chord - prefer chord for uppercase

    // Try to parse as chord symbol
    auto chord_info = parse_chord_symbol(chord_text);
    if (!chord_info.has_value()) {
        return std::nullopt;
    }

    // Must be followed by: end, whitespace, modifier, bracket, or other delimiter
    if (scan_pos < pattern_.size()) {
        char next = pattern_[scan_pos];
        if (!is_whitespace(next) &&
            next != '*' && next != '/' && next != '@' && next != '!' && next != '?' && next != '%' &&
            next != '[' && next != ']' && next != '<' && next != '>' &&
            next != '(' && next != ')' && next != '{' && next != '}' &&
            next != ',' && next != '|' && next != ':') {
            // Followed by something that's not a valid delimiter
            return std::nullopt;
        }
    }

    // Consume the chord token
    while (current_ < scan_pos) {
        advance();
    }

    // parse_chord_symbol already populated chord_info->intervals from the
    // canonical table — convert int → int8_t to match MiniChordData's slot.
    std::vector<std::int8_t> interval_vec(chord_info->intervals.begin(),
                                           chord_info->intervals.end());

    float velocity = 1.0f;

    // Check for :velocity suffix (e.g., Am:0.5)
    if (peek() == ':' && (is_digit(peek_next()) || peek_next() == '.')) {
        advance(); // consume ':'
        std::size_t vel_start = current_;
        if (peek() == '.') advance();
        while (is_digit(peek())) advance();
        if (pattern_[vel_start] != '.' && peek() == '.' && is_digit(peek_next())) {
            advance(); // consume '.'
            while (is_digit(peek())) advance();
        }
        std::string_view vel_text = pattern_.substr(vel_start, current_ - vel_start);
        std::string vel_buf(vel_text);
        char* vel_end = nullptr;
        double vel_val = std::strtod(vel_buf.c_str(), &vel_end);
        if (vel_end == vel_buf.c_str()) vel_val = 0.0;
        velocity = static_cast<float>(std::clamp(vel_val, 0.0, 1.0));
    }

    auto props = try_lex_record_suffix();

    MiniChordData chord_data{
        chord_info->root,
        chord_info->quality,
        static_cast<std::uint8_t>(chord_info->root_midi),
        std::move(interval_vec),
        velocity,
        std::move(props)
    };

    return make_token(MiniTokenType::ChordToken, std::move(chord_data));
}

MiniToken MiniLexer::lex_pitch() {
    // Character-by-character pitch parsing that handles microtonal modifiers (^, v, +, x)
    // Called only when looks_like_pitch() returned true, so we know this IS a pitch.
    char note_letter = advance();
    int accidental_std = 0;
    std::int8_t micro_offset = 0;

    // Parse modifier stream: standard accidentals (#, b, x) and microtonal
    // operators / aliases (^, v, +, d, \) per PRD §4.1–§4.2. `d` is the
    // Stein-Zimmermann inverted-flat alias and is ambiguous with note D /
    // sample names like "bd"; we only treat it as a modifier when the rest
    // of the token reaches an octave digit. looks_like_pitch() applies the
    // same rule so the gate stays consistent.
    auto chain_reaches_digit = [this](std::size_t scan) {
        while (scan < pattern_.size()) {
            char sc = pattern_[scan];
            if (is_digit(sc)) return true;
            if (sc == '#' || sc == 'b' || sc == 'x' ||
                sc == '^' || sc == 'v' || sc == '+' ||
                sc == 'd' || sc == '\\') {
                scan++;
            } else {
                return false;
            }
        }
        return false;
    };
    while (!is_at_end()) {
        char c = peek();
        if (c == '#')      { accidental_std++; advance(); }
        else if (c == 'b') { accidental_std--; advance(); }  // always flat in pitch context
        else if (c == 'x') { accidental_std += 2; advance(); }
        else if (c == '^') { micro_offset++; advance(); }
        else if (c == 'v') { micro_offset--; advance(); }
        else if (c == '+') { micro_offset++; advance(); }
        else if (c == '\\') { micro_offset--; advance(); }
        else if (c == 'd' && chain_reaches_digit(current_ + 1)) {
            micro_offset--; advance();
        }
        else break;
    }

    // Parse octave (0-9, default 4)
    int octave = 4;
    bool has_octave = false;
    if (!is_at_end() && is_digit(peek())) {
        has_octave = true;
        octave = advance() - '0';
        // Double-digit octave
        if (!is_at_end() && is_digit(peek())) {
            octave = octave * 10 + (advance() - '0');
        }
    }

    std::uint8_t midi = parse_pitch_to_midi(note_letter, accidental_std, octave);
    float velocity = 1.0f;

    // Check for :velocity suffix (e.g., c4:0.8)
    if (peek() == ':' && (is_digit(peek_next()) || peek_next() == '.')) {
        advance(); // consume ':'
        std::size_t vel_start = current_;
        if (peek() == '.') advance();
        while (is_digit(peek())) advance();
        if (pattern_[vel_start] != '.' && peek() == '.' && is_digit(peek_next())) {
            advance(); // consume '.'
            while (is_digit(peek())) advance();
        }
        std::string_view vel_text = pattern_.substr(vel_start, current_ - vel_start);
        std::string vel_buf(vel_text);
        char* vel_end = nullptr;
        double vel_val = std::strtod(vel_buf.c_str(), &vel_end);
        if (vel_end == vel_buf.c_str()) vel_val = 0.0;
        velocity = static_cast<float>(std::clamp(vel_val, 0.0, 1.0));
    }

    auto props = try_lex_record_suffix();
    return make_token(MiniTokenType::PitchToken,
                      MiniPitchData{midi, has_octave, velocity, micro_offset, std::move(props)});
}

std::vector<std::pair<std::string, float>> MiniLexer::try_lex_record_suffix() {
    // Phase 2 PRD §5.6: parse `{key:number(,key:number)*}` immediately after
    // a note token (no whitespace). Disambiguates from polymeter `{a b}%n`:
    // record-suffix `{` MUST be the very next char, AND its first content
    // tokens must be `identifier:number` form. If anything doesn't match,
    // we rewind and produce nothing (so polymeter's lex path can take over).
    std::vector<std::pair<std::string, float>> properties;
    if (peek() != '{') return properties;

    // Try to peek at the content: identifier `:` number ?
    std::size_t save = current_;
    advance();  // consume `{`

    while (!is_at_end()) {
        // Skip whitespace within the record body — minimal tolerance.
        while (!is_at_end() && is_whitespace(peek())) advance();

        if (peek() == '}') { advance(); return properties; }

        // Parse identifier (key).
        if (!is_alpha(peek())) {
            // Not a valid record-suffix; rewind.
            current_ = save;
            properties.clear();
            return properties;
        }
        std::size_t key_start = current_;
        while (!is_at_end() && (is_alpha(peek()) || is_digit(peek()) || peek() == '_')) {
            advance();
        }
        std::string key(pattern_.substr(key_start, current_ - key_start));

        while (!is_at_end() && is_whitespace(peek())) advance();
        if (peek() != ':') {
            // Not a record-suffix; rewind.
            current_ = save;
            properties.clear();
            return properties;
        }
        advance();  // consume `:`
        while (!is_at_end() && is_whitespace(peek())) advance();

        // Parse number value (optional leading `-` and `.`).
        std::size_t val_start = current_;
        if (peek() == '-' || peek() == '+') advance();
        bool has_digit = false;
        while (!is_at_end() && is_digit(peek())) { advance(); has_digit = true; }
        if (!is_at_end() && peek() == '.') {
            advance();
            while (!is_at_end() && is_digit(peek())) { advance(); has_digit = true; }
        }
        if (!has_digit) {
            current_ = save;
            properties.clear();
            return properties;
        }
        std::string num_buf(pattern_.substr(val_start, current_ - val_start));
        char* num_end = nullptr;
        double num = std::strtod(num_buf.c_str(), &num_end);
        properties.emplace_back(std::move(key), static_cast<float>(num));

        while (!is_at_end() && is_whitespace(peek())) advance();
        if (peek() == ',') { advance(); continue; }
        if (peek() == '}') { advance(); return properties; }

        // Malformed — rewind.
        current_ = save;
        properties.clear();
        return properties;
    }

    // Reached end without `}`.
    current_ = save;
    properties.clear();
    return properties;
}

MiniToken MiniLexer::lex_sample_only() {
    // Consume all alphanumeric characters
    while (!is_at_end()) {
        char c = peek();
        if (is_alpha(c) || is_digit(c) || c == '#') {
            advance();
        } else {
            break;
        }
    }

    std::string_view text = pattern_.substr(start_, current_ - start_);

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

    auto props = try_lex_record_suffix();
    return make_token(MiniTokenType::SampleToken,
                      MiniSampleData{std::string(text), variant, "", std::move(props)});
}

// Convenience function
std::pair<std::vector<MiniToken>, std::vector<Diagnostic>>
lex_mini(std::string_view pattern, SourceLocation base_location, MiniParseMode mode) {
    MiniLexer lexer(pattern, base_location, mode);
    auto tokens = lexer.lex_all();
    return {std::move(tokens), lexer.diagnostics()};
}

std::pair<std::vector<MiniToken>, std::vector<Diagnostic>>
lex_mini(std::string_view pattern, SourceLocation base_location, bool sample_only, bool curve_mode) {
    return lex_mini(pattern, base_location, mode_from_bools(sample_only, curve_mode));
}

} // namespace akkado
