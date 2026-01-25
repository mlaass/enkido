#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace akkado {

/// Information about a parsed chord symbol
struct ChordInfo {
    std::string root;           // Root note name: "C", "F#", "Bb"
    std::string quality;        // Chord quality: "", "m", "7", "M7", "dim", etc.
    int root_midi;              // MIDI note of root (default octave 4)
    std::vector<int> intervals; // Semitone intervals from root (e.g., [0, 4, 7] for major)
};

/// Parse a single chord symbol
/// Examples: "Am", "C7", "Fmaj7", "Gdim", "Bb+", "Dsus4"
/// @param symbol The chord symbol string
/// @return ChordInfo if parsing succeeded, nullopt otherwise
[[nodiscard]] std::optional<ChordInfo> parse_chord_symbol(std::string_view symbol);

/// Expand a chord to MIDI note numbers
/// @param chord The parsed chord info
/// @param octave Base octave for the root note (default 4)
/// @return Vector of MIDI note numbers
[[nodiscard]] std::vector<int> expand_chord(const ChordInfo& chord, int octave = 4);

/// Parse a chord pattern string containing multiple chords
/// Example: "Am C7 F G" -> vector of ChordInfo
/// @param pattern Space-separated chord symbols
/// @return Vector of parsed chords (empty for parse errors)
[[nodiscard]] std::vector<ChordInfo> parse_chord_pattern(std::string_view pattern);

/// Get root note MIDI number from note name
/// @param root_name Root note name (e.g., "C", "F#", "Bb")
/// @param octave Octave number (default 4)
/// @return MIDI note number, or -1 if invalid
[[nodiscard]] int root_name_to_midi(std::string_view root_name, int octave = 4);

} // namespace akkado
