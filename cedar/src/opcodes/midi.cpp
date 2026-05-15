#include "cedar/opcodes/midi.hpp"

#include <algorithm>
#include <cmath>

#include "cedar/dsp/constants.hpp"
#include "cedar/io/midi_sequence.hpp"
#include "cedar/vm/context.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/vm/state_pool.hpp"
#include "cedar/opcodes/dsp_state.hpp"  // for std::variant alternative

namespace cedar {

namespace {

// Float MIDI-note → frequency. Matches op_mtof at utility.hpp:140.
inline float midi_to_freq(std::uint8_t note) noexcept {
    return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);
}

// Per-block beat clock at the start of the block (sample 0). Matches the
// double-precision calculation execute_poly_block performs at vm.cpp:323-326
// so events emitted here with `time = block_start_beats` align exactly with
// the cycle_pos POLY then computes for the same block.
inline double block_start_beats(const ExecutionContext& ctx) noexcept {
    const double spb_d = (60.0 / static_cast<double>(ctx.bpm))
                       * static_cast<double>(ctx.sample_rate);
    if (spb_d <= 0.0) return 0.0;
    return static_cast<double>(ctx.global_sample_counter) / spb_d;
}

// Drain everything queued in the SPSC ring up to the writer's current
// position, emitting OutputEvents into `s.output`. Drop-newest overflow is
// the producer's policy in `push_midi_event`; the consumer just reads.
void drain_live_events_into_output(MidiQueueState& s,
                                   const ExecutionContext& ctx) {
    if (!s.ring || s.ring_capacity == 0) return;

    const float now_beats = static_cast<float>(block_start_beats(ctx));

    // Acquire-load the writer cursor so the payload writes are observed.
    const std::uint64_t w = s.write_pos.load(std::memory_order_acquire);
    std::uint64_t       r = s.read_pos.load(std::memory_order_relaxed);

    for (; r != w; ++r) {
        const MidiRawEvent& ev = s.ring[r % s.ring_capacity];

        // Channel filter: 0 = any; otherwise match 1-indexed MIDI channel.
        if (s.channel_filter != 0 &&
            static_cast<std::uint8_t>(ev.channel + 1u) != s.channel_filter) {
            continue;
        }

        const std::uint8_t status_high = ev.status & 0xF0u;
        const std::uint8_t note        = ev.d1 & 0x7Fu;

        const bool is_note_off =
            (status_high == 0x80u) ||
            (status_high == 0x90u && ev.d2 == 0);
        const bool is_note_on =
            (status_high == 0x90u && ev.d2 > 0);

        if (is_note_on) {
            // If a note-on for the same key is already held, the old event
            // is orphaned at its sentinel duration (will eventually be a
            // permanent "in the past" entry in output that POLY ignores).
            // Phase-N could add an explicit retrigger semantics.
            const float vel_norm = static_cast<float>(ev.d2) / 127.0f;
            const float freq     = midi_to_freq(note);

            const std::uint32_t idx_before = s.output.num_events;
            s.output.add(
                /*time*/      now_beats,
                /*duration*/  MidiQueueState::HELD_DURATION_SENTINEL,
                /*vals*/      &freq,
                /*count*/     1,
                /*velocity*/  vel_norm,
                /*type_id*/   0,
                /*src_off*/   0,
                /*src_len*/   0,
                /*chance*/    1.0f,
                /*midi_note*/ static_cast<float>(note));

            // OutputEvents::add silently drops if at capacity — only record
            // the held-note linkage when an event slot was actually written.
            if (s.output.num_events > idx_before) {
                s.held_note_to_event[note] =
                    static_cast<std::int32_t>(idx_before);
            }
        } else if (is_note_off) {
            const std::int32_t idx = s.held_note_to_event[note];
            if (idx >= 0 &&
                static_cast<std::uint32_t>(idx) < s.output.num_events) {
                auto& evt = s.output.events[idx];
                float dur = now_beats - evt.time;
                if (!(dur > 0.0f)) {
                    // Zero or negative duration (same-block note-on + off,
                    // or float-precision jitter at the boundary). Use a
                    // single-sample floor so POLY fires the gate-off in the
                    // current block instead of skipping it.
                    const float spb = ctx.samples_per_beat();
                    dur = (spb > 0.0f) ? (1.0f / spb) : 1.0e-6f;
                }
                evt.duration = dur;
                s.held_note_to_event[note] = -1;
            }
        }
        // All other status bytes silently discarded in Phase 1.
    }

    // Publish the consumer's progress. The producer only needs this if it
    // ever transitions from full → not-full; today it does not (drop-newest
    // means the producer never blocks), but maintaining the release here
    // keeps the protocol straightforward for the Phase-N producer policies
    // mentioned in the PRD.
    s.read_pos.store(r, std::memory_order_release);
}

// Compute one note's start/end in file-local beats based on the state's
// tempo mode. Follow mode uses a trivial division (engine BPM stretches
// the file); File mode honours the precomputed tempo-map beats.
inline double note_on_beat(const MidiNote& n,
                           const MidiSequence& seq,
                           MidiQueueState::TempoMode mode) noexcept {
    if (mode == MidiQueueState::TempoMode::File) {
        return static_cast<double>(n.beat_on_file);
    }
    const double tpq = static_cast<double>(seq.ticks_per_quarter);
    return tpq > 0.0 ? static_cast<double>(n.tick_on) / tpq : 0.0;
}
inline double note_off_beat(const MidiNote& n,
                            const MidiSequence& seq,
                            MidiQueueState::TempoMode mode) noexcept {
    if (mode == MidiQueueState::TempoMode::File) {
        return static_cast<double>(n.beat_off_file);
    }
    const double tpq = static_cast<double>(seq.ticks_per_quarter);
    return tpq > 0.0 ? static_cast<double>(n.tick_off) / tpq : 0.0;
}

// Total file length in beats for the current tempo mode. Follow mode uses
// `total_ticks / TPQ` (engine BPM scales it); File mode uses the
// precomputed `total_beats_file`.
inline double seq_total_beats(const MidiSequence& seq,
                              MidiQueueState::TempoMode mode) noexcept {
    if (mode == MidiQueueState::TempoMode::File) {
        return static_cast<double>(seq.total_beats_file);
    }
    const double tpq = static_cast<double>(seq.ticks_per_quarter);
    return tpq > 0.0 ? static_cast<double>(seq.total_ticks) / tpq : 0.0;
}

// Emit one note that falls inside the block-local window
// [play_head, play_head + block_beats) so its on/off both land in this block.
// The OutputEvent uses absolute beat time so POLY can place it sample-
// accurately within the block once that follow-up phase lands; v1's POLY
// block-boundary timing is unaffected.
inline void emit_file_note(MidiQueueState& s,
                           const MidiNote& n,
                           double on_beat_local,
                           double off_beat_local,
                           double play_head,
                           float  block_start_beat_abs) {
    const float freq = midi_to_freq(n.note);
    const float vel  = static_cast<float>(n.vel) / 127.0f;
    // POLY expects strictly-positive durations; if note_off_beat <=
    // note_on_beat (zero-tick degenerate or float-precision jitter), use
    // a tiny floor so the gate still fires for one sample.
    double dur_beats = off_beat_local - on_beat_local;
    if (!(dur_beats > 0.0)) dur_beats = 1.0e-6;
    const float abs_time =
        block_start_beat_abs + static_cast<float>(on_beat_local - play_head);
    s.output.add(
        /*time*/      abs_time,
        /*duration*/  static_cast<float>(dur_beats),
        /*vals*/      &freq,
        /*count*/     1,
        /*velocity*/  vel,
        /*type_id*/   0,
        /*src_off*/   0,
        /*src_len*/   0,
        /*chance*/    1.0f,
        /*midi_note*/ static_cast<float>(n.note));
}

// Scan all notes whose on-beat lands in [scan_lo, scan_hi). `play_head` is
// the file-local beat the OutputEvent times are emitted relative to;
// `block_start_beat_abs` is added so the result is in the absolute beat
// space POLY consumes. Channel filter is applied here (1-indexed match;
// 0 = any).
inline void scan_notes_into_block(MidiQueueState& s,
                                  const MidiSequence& seq,
                                  double scan_lo,
                                  double scan_hi,
                                  double play_head,
                                  float  block_start_beat_abs) {
    for (std::uint32_t i = 0; i < seq.num_notes; ++i) {
        const MidiNote& n = seq.notes[i];
        if (s.channel_filter != 0 &&
            static_cast<std::uint8_t>(n.channel + 1u) != s.channel_filter) {
            continue;
        }
        const double on_local  = note_on_beat (n, seq, s.tempo_mode);
        if (on_local < scan_lo || on_local >= scan_hi) continue;
        const double off_local = note_off_beat(n, seq, s.tempo_mode);
        emit_file_note(s, n, on_local, off_local, play_head,
                       block_start_beat_abs);
    }
}

// Phase 5: advance the file play head across one block, emitting any note
// that starts in [head, head + block_beats). Loop wraps the head modulo
// `total_beats` and emits a second segment to cover notes straddling the
// boundary. Both follow-mode and file-mode tempo are honoured via the
// helpers above.
void advance_file_seq_into_output(MidiQueueState& s,
                                  const ExecutionContext& ctx) {
    if (!s.file_seq || s.file_seq->num_notes == 0) return;
    const MidiSequence& seq = *s.file_seq;

    const double spb = static_cast<double>(ctx.samples_per_beat());
    if (!(spb > 0.0)) return;
    const double block_beats = static_cast<double>(BLOCK_SIZE) / spb;
    if (!(block_beats > 0.0)) return;

    const double total_beats = seq_total_beats(seq, s.tempo_mode);
    const float now_beats_abs = static_cast<float>(block_start_beats(ctx));

    // Express the block window in file-local beat coordinates as a single
    // [scan_lo, scan_hi) pair, then handle loop wrap by walking the window
    // in at most a few discrete segments. This avoids the
    // `remaining -= consumed` accumulator pattern, which was vulnerable to
    // sub-ULP residue keeping the loop alive after the block was fully
    // consumed (head + 0.005333 - head ≠ exactly 0.005333 across many
    // iterations).
    const double scan_lo_global = s.file_play_head_beats;
    double scan_hi_global = scan_lo_global + block_beats;

    if (!s.loop || !(total_beats > 0.0)) {
        // No loop: scan the whole window. If we crossed end-of-track, park
        // the play head at total_beats; subsequent blocks scan an empty
        // [total, total + block_beats) window and emit nothing.
        const double hi = (total_beats > 0.0)
            ? std::min<double>(scan_hi_global, total_beats)
            : scan_hi_global;
        scan_notes_into_block(s, seq, scan_lo_global, hi, scan_lo_global,
                              now_beats_abs);
        s.file_play_head_beats = (total_beats > 0.0)
            ? std::min<double>(scan_hi_global, total_beats)
            : scan_hi_global;
        return;
    }

    // Looping: walk the window in at most a fixed number of segments. The
    // iteration cap (sized for any realistic block:loop ratio) keeps the
    // loop bounded even if floating-point residue would otherwise leave
    // `remaining` infinitesimally positive — the previous remaining-based
    // termination was vulnerable to sub-ULP drift after many blocks.
    constexpr int kMaxSegments = 8;
    double cursor_local = std::fmod(scan_lo_global, total_beats);
    if (cursor_local < 0.0) cursor_local += total_beats;
    // Express the block end as a target in cumulative-beats space; we walk
    // segments by tracking how much of the block has been consumed, never
    // relying on a (block_beats - sum_of_consumes) accumulator.
    double consumed_total = 0.0;
    for (int i = 0; i < kMaxSegments; ++i) {
        const double remaining = block_beats - consumed_total;
        if (remaining <= 1e-12) break;
        const double seg_remaining = total_beats - cursor_local;
        const double consume = std::min<double>(remaining, seg_remaining);
        scan_notes_into_block(s, seq, cursor_local, cursor_local + consume,
                              cursor_local, now_beats_abs);
        consumed_total += consume;
        cursor_local += consume;
        if (cursor_local + 1e-12 >= total_beats) {
            cursor_local = 0.0;
            s.current_tempo_idx = 0;  // reserved
        }
    }
    s.file_play_head_beats = cursor_local;
}

}  // namespace

void op_midi_query(ExecutionContext& ctx, const Instruction& inst) {
    auto& s = ctx.states->get_or_create<MidiQueueState>(inst.state_id);

    drain_live_events_into_output(s, ctx);
    advance_file_seq_into_output(s, ctx);
}

}  // namespace cedar
