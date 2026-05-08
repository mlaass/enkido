#include "akkado/chord_parser.hpp"
#include "akkado/music_theory.hpp"  // canonical CHORD_INTERVALS + lookup_chord
#include <cctype>
#include <unordered_map>

namespace akkado {

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

    // Look up the quality intervals from the canonical table.
    const auto* intervals = lookup_chord(info.quality);
    if (intervals == nullptr) {
        // Unknown quality → default major triad. Reset quality to "" so
        // downstream consumers (voicing dict.qualities lookup, etc.) don't
        // try to match an override against a phantom string.
        info.quality = "";
        info.intervals = {0, 4, 7};
    } else {
        // Convert int8_t (storage) → int (ChordInfo public API).
        info.intervals.assign(intervals->begin(), intervals->end());
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
