#include "akkado/lex_primitives.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace akkado::lex_primitives {

NumericScanResult scan_number(CursorBase& cur, std::uint32_t parse_from,
                              NumericScanOpts o) {
    if (o.allow_sign && (cur.peek() == '+' || cur.peek() == '-')) {
        cur.advance();
    }

    bool has_dot = o.seen_dot;
    // Leading '.' (only ever reached with a digit following — every
    // caller's dispatch guard guarantees it).
    if (!has_dot && cur.peek() == '.' && is_digit(cur.peek_next())) {
        has_dot = true;
        cur.advance();
    }
    while (is_digit(cur.peek())) {
        cur.advance();
    }
    // Fractional part after digits.
    if (!has_dot && cur.peek() == '.' &&
        (o.greedy_dot || is_digit(cur.peek_next()))) {
        has_dot = true;
        cur.advance();
        while (is_digit(cur.peek())) {
            cur.advance();
        }
    }

    // Exponent — validated by lookahead so an invalid one ("1e+", "2ex")
    // is never consumed and the cursor/column stay untouched.
    bool has_exponent = false;
    if (o.allow_exponent && (cur.peek() == 'e' || cur.peek() == 'E')) {
        char n1 = cur.peek_next();
        bool valid = is_digit(n1) ||
                     ((n1 == '+' || n1 == '-') && is_digit(cur.peek_ahead(2)));
        if (valid) {
            has_exponent = true;
            cur.advance();  // e/E
            if (cur.peek() == '+' || cur.peek() == '-') cur.advance();
            while (is_digit(cur.peek())) cur.advance();
        }
    }

    std::string buf(cur.slice(parse_from));
    char* end = nullptr;
    double value = std::strtod(buf.c_str(), &end);
    return {value, !has_dot && !has_exponent, end != buf.c_str()};
}

float scan_velocity_suffix(CursorBase& cur) {
    if (cur.peek() != ':' ||
        !(is_digit(cur.peek_next()) || cur.peek_next() == '.')) {
        return 1.0f;
    }
    cur.advance();  // consume ':'
    const std::uint32_t vel_start = cur.current();
    const bool leading_dot = cur.peek() == '.';
    if (leading_dot) cur.advance();
    while (is_digit(cur.peek())) cur.advance();
    if (!leading_dot && cur.peek() == '.' && is_digit(cur.peek_next())) {
        cur.advance();
        while (is_digit(cur.peek())) cur.advance();
    }
    std::string buf(cur.slice(vel_start));
    char* end = nullptr;
    double value = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str()) value = 0.0;
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

std::uint8_t pitch_to_midi(char note_letter, int accidental, int octave) {
    // Note letter semitones: A=9, B=11, C=0, D=2, E=4, F=5, G=7
    static constexpr int semitones[] = {9, 11, 0, 2, 4, 5, 7};  // a..g
    char note_lower = note_letter | 0x20;
    int midi_note = (octave + 1) * 12 + semitones[note_lower - 'a'] + accidental;
    if (midi_note < 0) midi_note = 0;
    if (midi_note > 127) midi_note = 127;
    return static_cast<std::uint8_t>(midi_note);
}

} // namespace akkado::lex_primitives
