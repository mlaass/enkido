#include "cedar/io/midi_sequence.hpp"

#include "cedar/vm/audio_arena.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace cedar {

namespace {

// Cursor over the byte stream. Tracks position and gracefully fails on
// truncated input by returning a flag without throwing — the parser is
// called from host code (web upload / CLI URI resolve) and must never
// crash on malformed bytes.
struct Cursor {
    const std::uint8_t* p = nullptr;
    const std::uint8_t* end = nullptr;
    bool error = false;

    [[nodiscard]] bool eof() const { return p >= end; }
    [[nodiscard]] std::size_t remaining() const {
        return static_cast<std::size_t>(end - p);
    }

    std::uint8_t u8() {
        if (eof()) { error = true; return 0; }
        return *p++;
    }
    std::uint16_t u16_be() {
        if (remaining() < 2) { error = true; return 0; }
        std::uint16_t v = (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
        p += 2;
        return v;
    }
    std::uint32_t u32_be() {
        if (remaining() < 4) { error = true; return 0; }
        std::uint32_t v = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
                        | (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
        p += 4;
        return v;
    }
    // Variable-length quantity: 1-4 bytes, MSB = continuation.
    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) { error = true; return 0; }
            std::uint8_t b = *p++;
            value = (value << 7) | (b & 0x7Fu);
            if ((b & 0x80u) == 0) return value;
        }
        // 5-byte VLQ is malformed.
        error = true;
        return value;
    }
    void skip(std::size_t n) {
        if (remaining() < n) { error = true; p = end; return; }
        p += n;
    }
    Cursor sub(std::size_t n) {
        if (remaining() < n) { error = true; return Cursor{end, end, true}; }
        Cursor c{p, p + n, false};
        p += n;
        return c;
    }
};

// Collected per-track output. Tempo events from any track contribute to a
// shared tempo map; note-on/off are paired inside each track to produce
// complete MidiNote entries. CCs (PRD §7.4) include CC, PB, and channel-AT;
// each track's CCs are merged into the parent MidiSequence and sorted by tick.
struct CollectedTrack {
    std::vector<MidiNote>  notes;
    std::vector<MidiTempo> tempos;
    std::vector<MidiCC>    ccs;
    std::uint32_t          last_tick = 0;  // ticks reached at end of track
};

// Held-note lookup keyed by (channel << 7) | note. -1 means not held.
struct HeldTable {
    std::array<std::int32_t, 16 * 128> last_on_tick{};
    std::array<std::int32_t, 16 * 128> last_on_vel{};

    HeldTable() { last_on_tick.fill(-1); last_on_vel.fill(0); }

    [[nodiscard]] static std::size_t key(std::uint8_t ch, std::uint8_t note) {
        return std::size_t(ch & 0x0Fu) * 128u + std::size_t(note & 0x7Fu);
    }
};

void parse_track(Cursor& tc, CollectedTrack& out) {
    std::uint32_t current_tick = 0;
    std::uint8_t running_status = 0;
    HeldTable held;

    while (!tc.eof() && !tc.error) {
        std::uint32_t dt = tc.vlq();
        if (tc.error) break;
        current_tick += dt;

        if (tc.eof()) break;
        std::uint8_t status = *tc.p;

        if (status < 0x80u) {
            // Running status — reuse last channel-voice status byte.
            if (running_status == 0) { tc.error = true; break; }
            status = running_status;
        } else {
            tc.p++;  // consume the explicit status byte
            if (status < 0xF0u) {
                running_status = status;
            } else {
                // System (sysex) and meta events do NOT update running status.
                running_status = 0;
            }
        }

        if (status == 0xFFu) {
            // Meta event: type + VLQ length + data
            std::uint8_t meta_type = tc.u8();
            std::uint32_t mlen = tc.vlq();
            if (tc.error) break;
            if (meta_type == 0x51u) {
                // Set tempo: 3 bytes BE, microseconds per quarter note
                if (mlen >= 3 && tc.remaining() >= 3) {
                    std::uint32_t us = (std::uint32_t(tc.p[0]) << 16)
                                     | (std::uint32_t(tc.p[1]) << 8)
                                     |  std::uint32_t(tc.p[2]);
                    out.tempos.push_back(MidiTempo{
                        /*tick*/           current_tick,
                        /*us_per_quarter*/ us == 0 ? 500000u : us,
                        /*beats_before*/   0.0f});
                }
                tc.skip(mlen);
            } else if (meta_type == 0x2Fu) {
                // End of track. mlen is 0 per spec; skip whatever is there.
                tc.skip(mlen);
                out.last_tick = current_tick;
                return;
            } else {
                tc.skip(mlen);
            }
        } else if (status == 0xF0u || status == 0xF7u) {
            // Sysex: VLQ length + data
            std::uint32_t slen = tc.vlq();
            if (tc.error) break;
            tc.skip(slen);
        } else {
            const std::uint8_t high = status & 0xF0u;
            const std::uint8_t ch   = status & 0x0Fu;
            if (high == 0x90u || high == 0x80u) {
                std::uint8_t note = tc.u8() & 0x7Fu;
                std::uint8_t vel  = tc.u8() & 0x7Fu;
                if (tc.error) break;
                const bool is_off = (high == 0x80u) || vel == 0;
                const std::size_t k = HeldTable::key(ch, note);
                if (is_off) {
                    const std::int32_t on_tick = held.last_on_tick[k];
                    if (on_tick >= 0) {
                        out.notes.push_back(MidiNote{
                            /*tick_on*/  static_cast<std::uint32_t>(on_tick),
                            /*tick_off*/ current_tick,
                            /*beat_on_file*/  0.0f,
                            /*beat_off_file*/ 0.0f,
                            /*note*/    note,
                            /*vel*/     static_cast<std::uint8_t>(held.last_on_vel[k]),
                            /*channel*/ ch});
                        held.last_on_tick[k] = -1;
                    }
                } else {
                    held.last_on_tick[k] = static_cast<std::int32_t>(current_tick);
                    held.last_on_vel[k]  = vel;
                }
            } else if (high == 0xB0u) {
                // Control change: cc# (7-bit) + value (7-bit). PRD §7.4 keeps
                // these in the per-track CC list so op_midi_query can drain
                // them into pending_ccs[] for host-side route dispatch.
                std::uint8_t cc_num = tc.u8() & 0x7Fu;
                std::uint8_t value  = tc.u8() & 0x7Fu;
                if (tc.error) break;
                out.ccs.push_back(MidiCC{
                    /*tick*/         current_tick,
                    /*beat_on_file*/ 0.0f,
                    /*channel*/      ch,
                    /*value*/        value,
                    /*cc_num*/       static_cast<std::uint16_t>(cc_num)});
            } else if (high == 0xE0u) {
                // Pitch-bend: 14-bit LSB-first. v1 emits 7-bit precision —
                // store the high byte and let downstream re-pack as needed.
                std::uint8_t lsb = tc.u8() & 0x7Fu;
                std::uint8_t msb = tc.u8() & 0x7Fu;
                if (tc.error) break;
                (void)lsb;
                out.ccs.push_back(MidiCC{
                    /*tick*/         current_tick,
                    /*beat_on_file*/ 0.0f,
                    /*channel*/      ch,
                    /*value*/        msb,
                    /*cc_num*/       static_cast<std::uint16_t>(128u)});
            } else if (high == 0xD0u) {
                // Channel pressure / aftertouch: 1-byte payload.
                std::uint8_t value = tc.u8() & 0x7Fu;
                if (tc.error) break;
                out.ccs.push_back(MidiCC{
                    /*tick*/         current_tick,
                    /*beat_on_file*/ 0.0f,
                    /*channel*/      ch,
                    /*value*/        value,
                    /*cc_num*/       static_cast<std::uint16_t>(129u)});
            } else if (high == 0xA0u) {
                // Poly aftertouch: 2-byte payload; not surfaced in v1 (route
                // table has no per-note AT entry). Consume to keep alignment.
                tc.u8(); tc.u8();
            } else if (high == 0xC0u) {
                // Program change: 1-byte payload; ignored in v1.
                tc.u8();
            } else {
                // Unknown status byte — malformed file.
                tc.error = true;
                break;
            }
        }
    }

    // Flush any still-held notes — pair them with the track's last tick
    // so the resulting MidiNote has a non-zero duration. Without this a
    // file with a missing note-off would silently swallow the note.
    for (std::size_t k = 0; k < held.last_on_tick.size(); ++k) {
        const std::int32_t on_tick = held.last_on_tick[k];
        if (on_tick < 0) continue;
        const std::uint8_t ch   = static_cast<std::uint8_t>(k / 128u);
        const std::uint8_t note = static_cast<std::uint8_t>(k % 128u);
        out.notes.push_back(MidiNote{
            /*tick_on*/  static_cast<std::uint32_t>(on_tick),
            /*tick_off*/ current_tick,
            /*beat_on_file*/  0.0f,
            /*beat_off_file*/ 0.0f,
            /*note*/    note,
            /*vel*/     static_cast<std::uint8_t>(held.last_on_vel[k]),
            /*channel*/ ch});
    }
    out.last_tick = current_tick;
}

}  // namespace

MidiSequence* parse_smf(const std::uint8_t* data, std::size_t len, AudioArena& arena) {
    if (!data || len < 14) return nullptr;  // header must fit
    Cursor c{data, data + len, false};

    // Header chunk
    if (c.u8() != 'M' || c.u8() != 'T' || c.u8() != 'h' || c.u8() != 'd') return nullptr;
    const std::uint32_t hlen = c.u32_be();
    if (c.error || hlen < 6) return nullptr;
    Cursor hc = c.sub(hlen);
    if (c.error) return nullptr;
    const std::uint16_t format = hc.u16_be();
    const std::uint16_t ntrks  = hc.u16_be();
    const std::uint16_t division = hc.u16_be();
    if (format == 2u) return nullptr;          // explicitly unsupported
    if (format > 2u) return nullptr;
    if (ntrks == 0u) return nullptr;
    if (division & 0x8000u) return nullptr;    // SMPTE timing not supported
    const std::uint16_t tpq = division ? division : 480u;

    // Process each MTrk chunk.
    std::vector<CollectedTrack> tracks;
    tracks.reserve(ntrks);
    std::uint32_t max_track_ticks = 0;

    for (std::uint16_t t = 0; t < ntrks; ++t) {
        if (c.remaining() < 8) return nullptr;
        if (c.u8() != 'M' || c.u8() != 'T' || c.u8() != 'r' || c.u8() != 'k') {
            // Skip unknown chunk types per SMF spec (length still follows).
            const std::uint32_t skip_len = c.u32_be();
            c.skip(skip_len);
            if (c.error) return nullptr;
            --t;  // re-loop without consuming a track slot
            continue;
        }
        const std::uint32_t tlen = c.u32_be();
        Cursor tc = c.sub(tlen);
        if (c.error) return nullptr;
        CollectedTrack track;
        parse_track(tc, track);
        if (tc.error) return nullptr;
        max_track_ticks = std::max(max_track_ticks, track.last_tick);
        tracks.push_back(std::move(track));
    }

    // Merge tempos from all tracks; sort by tick. Ensure tempos[0].tick == 0
    // with a synthesized default-tempo entry if the file declares no tempo
    // before the first note.
    std::vector<MidiTempo> merged_tempos;
    for (const auto& tr : tracks) {
        merged_tempos.insert(merged_tempos.end(), tr.tempos.begin(), tr.tempos.end());
    }
    std::sort(merged_tempos.begin(), merged_tempos.end(),
              [](const MidiTempo& a, const MidiTempo& b) { return a.tick < b.tick; });
    if (merged_tempos.empty() || merged_tempos.front().tick > 0u) {
        merged_tempos.insert(merged_tempos.begin(),
                             MidiTempo{0, 500000u, 0.0f});
    }

    // Precompute beats_before for each tempo segment. The tempo's
    // us_per_quarter is not needed for beat math (TPQ is the only
    // tick-to-beat conversion factor), but we keep it for downstream
    // analytics / debugging.
    for (std::size_t i = 1; i < merged_tempos.size(); ++i) {
        const auto& prev = merged_tempos[i - 1];
        merged_tempos[i].beats_before =
            prev.beats_before
          + static_cast<float>(static_cast<double>(merged_tempos[i].tick - prev.tick)
                               / static_cast<double>(tpq));
    }

    // Merge notes from all tracks; sort by tick_on.
    std::vector<MidiNote> merged_notes;
    for (const auto& tr : tracks) {
        merged_notes.insert(merged_notes.end(), tr.notes.begin(), tr.notes.end());
    }
    std::sort(merged_notes.begin(), merged_notes.end(),
              [](const MidiNote& a, const MidiNote& b) {
                  if (a.tick_on != b.tick_on) return a.tick_on < b.tick_on;
                  return a.note < b.note;
              });

    // Precompute beat_on_file / beat_off_file using the tempo map.
    auto tick_to_beat_file = [&](std::uint32_t tick) -> float {
        // Linear scan; tempos.size() is typically < 10. Acceptable for
        // a one-shot parse.
        std::size_t idx = 0;
        for (std::size_t i = 1; i < merged_tempos.size(); ++i) {
            if (merged_tempos[i].tick <= tick) idx = i;
            else break;
        }
        const auto& t = merged_tempos[idx];
        const double beats_in_seg =
            static_cast<double>(tick - t.tick) / static_cast<double>(tpq);
        return t.beats_before + static_cast<float>(beats_in_seg);
    };

    for (auto& n : merged_notes) {
        n.beat_on_file  = tick_to_beat_file(n.tick_on);
        n.beat_off_file = tick_to_beat_file(n.tick_off);
    }
    const float total_beats_file = tick_to_beat_file(max_track_ticks);

    // Merge CCs from every track and sort by tick. Per PRD §7.4 the file-CC
    // drain scans the array within the play-head window; sorted order keeps
    // the scan output chronological so dispatch matches the source file.
    std::vector<MidiCC> merged_ccs;
    for (const auto& tr : tracks) {
        merged_ccs.insert(merged_ccs.end(), tr.ccs.begin(), tr.ccs.end());
    }
    std::sort(merged_ccs.begin(), merged_ccs.end(),
              [](const MidiCC& a, const MidiCC& b) {
                  if (a.tick != b.tick) return a.tick < b.tick;
                  return a.cc_num < b.cc_num;
              });
    for (auto& cc : merged_ccs) {
        cc.beat_on_file = tick_to_beat_file(cc.tick);
    }

    // Allocate arena slices and copy. Each struct's size is rounded up
    // to a multiple of sizeof(float) so the bump allocator's next call
    // stays aligned.
    auto arena_alloc = [&](std::size_t bytes) -> void* {
        const std::size_t floats = (bytes + sizeof(float) - 1) / sizeof(float);
        return arena.allocate(floats);
    };

    auto* seq_mem = arena_alloc(sizeof(MidiSequence));
    if (!seq_mem) return nullptr;
    auto* seq = new (seq_mem) MidiSequence{};

    if (!merged_notes.empty()) {
        auto* notes_mem = arena_alloc(sizeof(MidiNote) * merged_notes.size());
        if (!notes_mem) return nullptr;
        std::memcpy(notes_mem, merged_notes.data(),
                    sizeof(MidiNote) * merged_notes.size());
        seq->notes = reinterpret_cast<MidiNote*>(notes_mem);
        seq->num_notes = static_cast<std::uint32_t>(merged_notes.size());
    }
    if (!merged_tempos.empty()) {
        auto* tempos_mem = arena_alloc(sizeof(MidiTempo) * merged_tempos.size());
        if (!tempos_mem) return nullptr;
        std::memcpy(tempos_mem, merged_tempos.data(),
                    sizeof(MidiTempo) * merged_tempos.size());
        seq->tempos = reinterpret_cast<MidiTempo*>(tempos_mem);
        seq->num_tempos = static_cast<std::uint32_t>(merged_tempos.size());
    }
    if (!merged_ccs.empty()) {
        auto* ccs_mem = arena_alloc(sizeof(MidiCC) * merged_ccs.size());
        if (!ccs_mem) return nullptr;
        std::memcpy(ccs_mem, merged_ccs.data(),
                    sizeof(MidiCC) * merged_ccs.size());
        seq->ccs = reinterpret_cast<MidiCC*>(ccs_mem);
        seq->num_ccs = static_cast<std::uint32_t>(merged_ccs.size());
    }
    seq->ticks_per_quarter = tpq;
    seq->total_ticks = max_track_ticks;
    seq->total_beats_file = total_beats_file;

    return seq;
}

}  // namespace cedar
