#include "akkado/chord_parser.hpp"
#include <cctype>
#include <unordered_map>

namespace akkado {

// Chord quality intervals (semitones from root)
static const std::unordered_map<std::string, std::vector<int>> CHORD_QUALITIES = {
    // Triads
    {"", {0, 4, 7}},           // Major
    {"maj", {0, 4, 7}},        // Major (explicit)
    {"M", {0, 4, 7}},          // Major (alternate)
    {"m", {0, 3, 7}},          // Minor
    {"min", {0, 3, 7}},        // Minor (explicit)
    {"-", {0, 3, 7}},          // Minor (alternate)
    {"dim", {0, 3, 6}},        // Diminished
    {"o", {0, 3, 6}},          // Diminished (alternate)
    {"aug", {0, 4, 8}},        // Augmented
    {"+", {0, 4, 8}},          // Augmented (alternate)
    {"sus2", {0, 2, 7}},       // Suspended 2nd
    {"sus4", {0, 5, 7}},       // Suspended 4th
    {"sus", {0, 5, 7}},        // Suspended (defaults to sus4)

    // Seventh chords
    {"7", {0, 4, 7, 10}},      // Dominant 7th
    {"dom7", {0, 4, 7, 10}},   // Dominant 7th (explicit)
    {"M7", {0, 4, 7, 11}},     // Major 7th
    {"maj7", {0, 4, 7, 11}},   // Major 7th (explicit)
    {"^", {0, 4, 7, 11}},      // Major 7th (Strudel-style)
    {"^7", {0, 4, 7, 11}},     // Major 7th (Strudel-style)
    {"m7", {0, 3, 7, 10}},     // Minor 7th
    {"min7", {0, 3, 7, 10}},   // Minor 7th (explicit)
    {"-7", {0, 3, 7, 10}},     // Minor 7th (alternate)
    {"dim7", {0, 3, 6, 9}},    // Diminished 7th
    {"o7", {0, 3, 6, 9}},      // Diminished 7th (alternate)
    {"m7b5", {0, 3, 6, 10}},   // Half-diminished 7th
    {"0", {0, 3, 6, 10}},      // Half-diminished 7th (alternate)
    {"aug7", {0, 4, 8, 10}},   // Augmented 7th
    {"+7", {0, 4, 8, 10}},     // Augmented 7th (alternate)
    {"mM7", {0, 3, 7, 11}},    // Minor-major 7th
    {"m^7", {0, 3, 7, 11}},    // Minor-major 7th (alternate)

    // Extended chords
    {"6", {0, 4, 7, 9}},       // Major 6th
    {"m6", {0, 3, 7, 9}},      // Minor 6th
    {"9", {0, 4, 7, 10, 14}},  // Dominant 9th
    {"M9", {0, 4, 7, 11, 14}}, // Major 9th
    {"maj9", {0, 4, 7, 11, 14}}, // Major 9th (explicit)
    {"m9", {0, 3, 7, 10, 14}}, // Minor 9th
    {"add9", {0, 4, 7, 14}},   // Add 9
    {"add2", {0, 2, 4, 7}},    // Add 2 (same as add9 but 2nd octave)
    {"11", {0, 4, 7, 10, 14, 17}}, // Dominant 11th
    {"m11", {0, 3, 7, 10, 14, 17}}, // Minor 11th
    {"13", {0, 4, 7, 10, 14, 21}}, // Dominant 13th

    // Power chord
    {"5", {0, 7}},             // Power chord (no 3rd)
};

// Note name semitone offsets (relative to C)
static const std::unordered_map<char, int> NOTE_SEMITONES = {
    {'C', 0}, {'c', 0},
    {'D', 2}, {'d', 2},
    {'E', 4}, {'e', 4},
    {'F', 5}, {'f', 5},
    {'G', 7}, {'g', 7},
    {'A', 9}, {'a', 9},
    {'B', 11}, {'b', 11},
};

int root_name_to_midi(std::string_view root_name, int octave) {
    if (root_name.empty()) return -1;

    // Parse note letter
    char note = root_name[0];
    auto it = NOTE_SEMITONES.find(note);
    if (it == NOTE_SEMITONES.end()) {
        return -1;  // Invalid note letter
    }

    int semitone = it->second;
    int accidental = 0;

    // Parse accidentals
    for (std::size_t i = 1; i < root_name.size(); ++i) {
        char c = root_name[i];
        if (c == '#') {
            accidental++;
        } else if (c == 'b') {
            accidental--;
        } else {
            return -1;  // Unexpected character
        }
    }

    // MIDI note: (octave + 1) * 12 + semitone + accidental
    int midi = (octave + 1) * 12 + semitone + accidental;

    // Clamp to valid MIDI range
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;

    return midi;
}

std::optional<ChordInfo> parse_chord_symbol(std::string_view symbol) {
    if (symbol.empty()) {
        return std::nullopt;
    }

    ChordInfo info;
    std::size_t pos = 0;

    // Parse root note letter
    if (!std::isalpha(static_cast<unsigned char>(symbol[0]))) {
        return std::nullopt;
    }

    // Convert to uppercase for consistency
    info.root = static_cast<char>(std::toupper(static_cast<unsigned char>(symbol[0])));
    pos = 1;

    // Parse accidentals (# or b)
    while (pos < symbol.size() && (symbol[pos] == '#' || symbol[pos] == 'b')) {
        info.root += symbol[pos];
        pos++;
    }

    // The rest is the quality
    info.quality = std::string(symbol.substr(pos));

    // Look up the quality intervals
    auto quality_it = CHORD_QUALITIES.find(info.quality);
    if (quality_it == CHORD_QUALITIES.end()) {
        // Unknown quality - default to major triad
        info.quality = "";
        info.intervals = {0, 4, 7};
    } else {
        info.intervals = quality_it->second;
    }

    // Calculate root MIDI note (default octave 4)
    info.root_midi = root_name_to_midi(info.root, 4);
    if (info.root_midi < 0) {
        return std::nullopt;
    }

    return info;
}

std::vector<int> expand_chord(const ChordInfo& chord, int octave) {
    std::vector<int> notes;
    notes.reserve(chord.intervals.size());

    int base_midi = root_name_to_midi(chord.root, octave);
    if (base_midi < 0) {
        return notes;  // Return empty on error
    }

    for (int interval : chord.intervals) {
        int note = base_midi + interval;
        // Clamp to valid MIDI range
        if (note >= 0 && note <= 127) {
            notes.push_back(note);
        }
    }

    return notes;
}

std::vector<ChordInfo> parse_chord_pattern(std::string_view pattern) {
    std::vector<ChordInfo> chords;

    // Split by whitespace
    std::size_t start = 0;
    std::size_t end = 0;

    while (start < pattern.size()) {
        // Skip leading whitespace
        while (start < pattern.size() && std::isspace(static_cast<unsigned char>(pattern[start]))) {
            start++;
        }

        if (start >= pattern.size()) {
            break;
        }

        // Find end of chord symbol
        end = start;
        while (end < pattern.size() && !std::isspace(static_cast<unsigned char>(pattern[end]))) {
            end++;
        }

        // Parse this chord
        std::string_view chord_str = pattern.substr(start, end - start);
        auto chord = parse_chord_symbol(chord_str);
        if (chord) {
            chords.push_back(*chord);
        }

        start = end;
    }

    return chords;
}

} // namespace akkado
