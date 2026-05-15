#pragma once

#include <cstddef>
#include <cstdint>

namespace cedar {

class AudioArena;

// One note event extracted from an SMF. Ticks are file-native; the beat
// fields are precomputed at parse time so `op_midi_query` never walks the
// tempo map. `beat_on_file` / `beat_off_file` honor the SMF tempo map
// (used when MidiQueueState::tempo_mode == File). For Follow mode, the
// caller computes `tick / ticks_per_quarter` directly — that formula is
// trivial and trades one division per emit for not duplicating the field.
struct MidiNote {
    std::uint32_t tick_on    = 0;
    std::uint32_t tick_off   = 0;
    float         beat_on_file  = 0.0f;
    float         beat_off_file = 0.0f;
    std::uint8_t  note    = 0;
    std::uint8_t  vel     = 0;
    std::uint8_t  channel = 0;   // 0..15
};

// One tempo entry from the SMF. `beats_before` is the number of beats that
// elapse before this tempo segment under the per-quarter timing, precomputed
// at parse time. Used to translate tick → beat in `tempo_mode == File`.
struct MidiTempo {
    std::uint32_t tick           = 0;
    std::uint32_t us_per_quarter = 500000;  // SMF default = 120 BPM
    float         beats_before   = 0.0f;
};

// Whole parsed file. Arena-owned; do not delete. Notes are sorted by
// `tick_on` ascending; tempos are sorted by `tick` ascending with an
// implicit `tempos[0].tick == 0` entry when the file does not declare one.
struct MidiSequence {
    MidiNote*     notes   = nullptr;
    std::uint32_t num_notes = 0;
    MidiTempo*    tempos  = nullptr;
    std::uint32_t num_tempos = 0;
    std::uint16_t ticks_per_quarter = 480;
    std::uint32_t total_ticks = 0;
    // Total length in beats under the embedded tempo map. Used by the
    // looping play head when `tempo_mode == File`. For Follow mode the
    // caller derives `total_ticks / ticks_per_quarter` directly.
    float         total_beats_file = 0.0f;
};

// Parse an SMF bytestream into a `MidiSequence` allocated from `arena`.
//
// Supported:
//   - Format 0 (single track) and format 1 (multi-track sharing tempo map)
//   - Variable-length quantities for delta-times and event lengths
//   - Running status across channel-voice messages
//   - Note-on (0x90, velocity 0 treated as note-off per MIDI spec)
//   - Note-off (0x80)
//   - Meta 0x51 (set tempo)
//   - Meta 0x2F (end of track)
//
// Discarded silently (parsed and skipped so subsequent events stay aligned):
//   - All other meta events
//   - Sysex 0xF0..0xF7
//   - Channel pressure, poly aftertouch, CC, program change, pitch-bend
//
// Returns nullptr on:
//   - Format-2 files
//   - Malformed headers / truncated input
//   - Out-of-arena allocations
MidiSequence* parse_smf(const std::uint8_t* data, std::size_t len, AudioArena& arena);

}  // namespace cedar
