#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace akkado::voicing {

/// Voicing modes: how to position a chord relative to an anchor MIDI note.
/// Per PRD §5.4 step 2.
enum class Mode : std::uint8_t {
    Below,  // all notes ≤ anchor
    Above,  // all notes ≥ anchor
    Duck,   // closest to anchor avoiding the anchor itself
    Root,   // root in bass octave, rest near anchor
};

/// Built-in and user-registered voicing dictionaries map a (sub)set of
/// chord-quality names to algorithmic transforms applied to a chord's
/// raw interval list. Built-ins are implemented algorithmically; user
/// dictionaries via addVoicings() are stored as explicit interval maps.
struct VoicingDict {
    // Per-quality interval overrides (from user-registered dicts).
    // Empty for the built-in dicts which compute intervals algorithmically.
    std::unordered_map<std::string, std::vector<int>> qualities;
    // Built-in transform: 0 = identity (close), 1 = open, 2 = drop2, 3 = drop3,
    // -1 = quality-table only (use qualities map; no algorithmic fallback).
    int builtin_kind = 0;
};

/// Parse a note-name anchor like "c4", "C#5", "Bb3" into a MIDI note number.
/// Returns nullopt for unparseable input.
std::optional<int> parse_anchor(std::string_view s);

/// Parse a mode name into the Mode enum. Accepts "below", "above", "duck",
/// "root" (case-insensitive). Returns nullopt for unrecognized modes.
std::optional<Mode> parse_mode(std::string_view s);

/// One chord event for the voice_chords pipeline. Inputs are the raw
/// (root_midi, intervals) describing the chord; output (in voice_chords)
/// is filled with the resolved per-voice MIDI notes.
struct ChordSpec {
    int root_midi = 60;
    std::vector<int> intervals;  // semitones from root, in source order
    // Chord quality string ("M", "m", "7", "M7", "m7", "dim", ...) for
    // dict.qualities lookup in voice_chords. Empty disables the override
    // (parser intervals are used).
    std::string quality;
};

/// Apply mode + anchor + (optional) voicing dict + greedy voice leading
/// to a sequence of chord specs. Returns one MIDI-note vector per chord
/// (same length as input), already voice-led to minimize total movement
/// from the previous chord. Per PRD §5.4 algorithm.
///
/// @param chords Input chord progression.
/// @param anchor_midi Anchor MIDI note (e.g., 60 for c4).
/// @param mode Positioning mode.
/// @param dict Optional voicing dictionary; nullptr selects built-in close.
std::vector<std::vector<int>> voice_chords(
    const std::vector<ChordSpec>& chords,
    int anchor_midi,
    Mode mode,
    const VoicingDict* dict);

/// Look up a voicing dictionary by name. Returns built-ins ("close", "open",
/// "drop2", "drop3") and user-registered dicts. Returns nullptr if name is
/// not recognized.
const VoicingDict* lookup_voicing(std::string_view name);

/// Register a user-defined voicing dictionary. Process-lifetime global
/// (mirrors param() registry). Subsequent calls with the same name
/// overwrite (consistent with hot-reload workflow).
void register_voicing(std::string_view name, VoicingDict dict);

} // namespace akkado::voicing
