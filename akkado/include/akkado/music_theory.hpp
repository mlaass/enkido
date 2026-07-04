#pragma once

#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace akkado {

/// Canonical chord-quality interval table. Single source of truth shared
/// by parse_chord_symbol() (chord_parser.cpp) and the mini-notation chord
/// lexer. Documented in docs/grammar/chord-grammar.md — keep in sync.
inline const std::unordered_map<std::string_view, std::vector<std::int8_t>> CHORD_INTERVALS = {
    // Triads
    {"",      {0, 4, 7}},        // Major (empty quality — default)
    {"M",     {0, 4, 7}},        // Major (alternate)
    {"maj",   {0, 4, 7}},        // Major (explicit)
    {"m",     {0, 3, 7}},        // Minor
    {"min",   {0, 3, 7}},        // Minor (explicit)
    {"-",     {0, 3, 7}},        // Minor (alternate)
    {"dim",   {0, 3, 6}},        // Diminished
    {"o",     {0, 3, 6}},        // Diminished (alternate)
    {"aug",   {0, 4, 8}},        // Augmented
    {"+",     {0, 4, 8}},        // Augmented (alternate)
    {"sus",   {0, 5, 7}},        // Suspended (defaults to sus4)
    {"sus2",  {0, 2, 7}},        // Suspended 2nd
    {"sus4",  {0, 5, 7}},        // Suspended 4th

    // Seventh chords
    {"7",     {0, 4, 7, 10}},    // Dominant 7th
    {"dom7",  {0, 4, 7, 10}},    // Dominant 7th (explicit)
    {"M7",    {0, 4, 7, 11}},    // Major 7th
    {"maj7",  {0, 4, 7, 11}},    // Major 7th (explicit)
    {"^",     {0, 4, 7, 11}},    // Major 7th (Strudel-style)
    {"^7",    {0, 4, 7, 11}},    // Major 7th (Strudel-style)
    {"m7",    {0, 3, 7, 10}},    // Minor 7th
    {"min7",  {0, 3, 7, 10}},    // Minor 7th (explicit)
    {"-7",    {0, 3, 7, 10}},    // Minor 7th (alternate)
    {"dim7",  {0, 3, 6, 9}},     // Diminished 7th
    {"o7",    {0, 3, 6, 9}},     // Diminished 7th (alternate)
    {"m7b5",  {0, 3, 6, 10}},    // Half-diminished 7th
    {"0",     {0, 3, 6, 10}},    // Half-diminished 7th (alternate)
    {"aug7",  {0, 4, 8, 10}},    // Augmented 7th
    {"+7",    {0, 4, 8, 10}},    // Augmented 7th (alternate)
    {"mM7",   {0, 3, 7, 11}},    // Minor-major 7th
    {"m^7",   {0, 3, 7, 11}},    // Minor-major 7th (alternate)
    {"minmaj7", {0, 3, 7, 11}},  // Minor-major 7th (explicit)

    // Sixth chords
    {"6",     {0, 4, 7, 9}},     // Major 6th
    {"m6",    {0, 3, 7, 9}},     // Minor 6th
    {"min6",  {0, 3, 7, 9}},     // Minor 6th (explicit)

    // Extended chords
    {"9",     {0, 4, 7, 10, 14}},        // Dominant 9th
    {"M9",    {0, 4, 7, 11, 14}},        // Major 9th
    {"maj9",  {0, 4, 7, 11, 14}},        // Major 9th (explicit)
    {"m9",    {0, 3, 7, 10, 14}},        // Minor 9th
    {"min9",  {0, 3, 7, 10, 14}},        // Minor 9th (explicit)
    {"add9",  {0, 4, 7, 14}},            // Add 9
    {"add2",  {0, 2, 4, 7}},             // Add 2 (same as add9 but 2nd octave)
    {"11",    {0, 4, 7, 10, 14, 17}},    // Dominant 11th
    {"m11",   {0, 3, 7, 10, 14, 17}},    // Minor 11th
    {"13",    {0, 4, 7, 10, 14, 21}},    // Dominant 13th

    // Power chord
    {"5",     {0, 7}},           // Power chord (root + fifth, no 3rd)
};

/// Look up chord intervals by name
/// @return Pointer to interval vector, or nullptr if not found
inline const std::vector<std::int8_t>* lookup_chord(std::string_view name) {
    auto it = CHORD_INTERVALS.find(name);
    if (it != CHORD_INTERVALS.end()) {
        return &it->second;
    }
    return nullptr;
}

/// Pitch class (0-11) of a note-name string ("d", "d#2", "eb") — octave
/// digits are parsed and ignored (key quantization is octave-agnostic).
/// @return -1 if the string is not a note name.
/// Used by the user-defined key() overload (prd-scale-quantize §4.6).
inline int note_name_pitch_class(std::string_view s) {
    if (s.empty()) return -1;
    static constexpr int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a..g
    const char c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(s[0])));
    if (c < 'a' || c > 'g') return -1;
    int pc = base[c - 'a'];
    std::size_t i = 1;
    for (; i < s.size(); ++i) {
        if (s[i] == '#')      pc += 1;
        else if (s[i] == 'b') pc -= 1;
        else break;
    }
    for (; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return -1;
    }
    return ((pc % 12) + 12) % 12;
}

/// 12-entry nearest-tone delta table for scale quantization: entry[pc] is
/// the signed-semitone move from input pitch class pc to the nearest pitch
/// class of the scale {root_pc + i mod 12 : i in intervals}; an exact tie
/// snaps to the LOWER tone (prd-scale-quantize §11.4). Intervals coerce
/// mod 12 — no validation by design (coerce-don't-fail; audit 2026-07-04).
/// An empty interval set yields all zeros (identity), matching the
/// unknown-scale-name fall-through contract. Mirrors keyDeltaTable in
/// web/scripts/generate-scale-quantize.ts.
inline std::vector<double> compute_key_deltas(
    int root_pc, const std::vector<double>& intervals) {
    bool in_p[12] = {};
    for (double iv : intervals) {
        in_p[(((root_pc + static_cast<int>(iv)) % 12) + 12) % 12] = true;
    }
    std::vector<double> deltas(12, 0.0);
    for (int pc = 0; pc < 12; ++pc) {
        if (in_p[pc]) continue;
        for (int d = 1; d <= 6; ++d) {
            if (in_p[(pc - d + 12) % 12]) { deltas[pc] = -d; break; }
            if (in_p[(pc + d) % 12])      { deltas[pc] = d;  break; }
        }
    }
    return deltas;
}

} // namespace akkado
