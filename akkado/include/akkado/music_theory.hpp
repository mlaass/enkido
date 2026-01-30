#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace akkado {

/// Common chord intervals (semitones from root)
inline const std::unordered_map<std::string_view, std::vector<std::int8_t>> CHORD_INTERVALS = {
    // Triads
    {"",      {0, 4, 7}},        // Major (empty quality - default)
    {"m",     {0, 3, 7}},        // Minor (short form)
    {"maj",   {0, 4, 7}},        // Major
    {"min",   {0, 3, 7}},        // Minor
    {"dim",   {0, 3, 6}},        // Diminished
    {"aug",   {0, 4, 8}},        // Augmented
    {"sus2",  {0, 2, 7}},        // Suspended 2nd
    {"sus4",  {0, 5, 7}},        // Suspended 4th

    // Seventh chords
    {"maj7",  {0, 4, 7, 11}},    // Major 7th
    {"min7",  {0, 3, 7, 10}},    // Minor 7th
    {"m7",    {0, 3, 7, 10}},    // Minor 7th (short form, e.g., Am7)
    {"dom7",  {0, 4, 7, 10}},    // Dominant 7th (also just "7")
    {"7",     {0, 4, 7, 10}},    // Dominant 7th (short form)
    {"dim7",  {0, 3, 6, 9}},     // Diminished 7th
    {"m7b5",  {0, 3, 6, 10}},    // Half-diminished (minor 7 flat 5)
    {"minmaj7", {0, 3, 7, 11}},  // Minor-major 7th

    // Extended chords
    {"9",     {0, 4, 7, 10, 14}},     // Dominant 9th
    {"maj9",  {0, 4, 7, 11, 14}},     // Major 9th
    {"min9",  {0, 3, 7, 10, 14}},     // Minor 9th
    {"add9",  {0, 4, 7, 14}},         // Add 9

    // Sixth chords
    {"6",     {0, 4, 7, 9}},     // Major 6th
    {"min6",  {0, 3, 7, 9}},     // Minor 6th

    // Power chord
    {"5",     {0, 7}},           // Power chord (root + fifth)
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
