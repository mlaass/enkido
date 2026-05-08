#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace akkado {

/// Canonical chord-quality interval table. Single source of truth shared
/// by parse_chord_symbol() (chord_parser.cpp), the mini-notation chord
/// lexer, and the apostrophe-syntax chord-literal lexer.
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

} // namespace akkado
