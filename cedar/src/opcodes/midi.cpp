#include "cedar/opcodes/midi.hpp"

#include <algorithm>
#include <cmath>

#include "cedar/dsp/constants.hpp"
#include "cedar/io/midi_sequence.hpp"
#include "cedar/vm/buffer_pool.hpp"
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

// PRD prd-midi-input §7.5: bake the OutputEvents touched by this block into
// per-sample gate/freq/vel/trig buffers using last-note-wins mono priority.
// Buffer indices ride on the MIDI_QUERY instruction (inst.inputs[0..3]);
// 0xFFFF means `midi()` wasn't bound via `as e` and the fill is skipped.
//
// Held-stack semantics: every note-on pushes onto a stack of currently-
// sounding notes; every note-off removes the matching entry. The mix-side
// gate stays 1.0 as long as the stack is non-empty (legato-style fallback);
// freq/vel track the most-recently-pushed entry. Trig fires one sample at
// each new note-on. The stack persists across blocks, so a chord held
// through a 1-second buffer underrun continues sounding the top note.
void fill_mono_buffers(MidiQueueState& s,
                       const ExecutionContext& ctx,
                       const Instruction& inst) {
    const std::uint16_t gate_buf_idx = inst.inputs[0];
    const std::uint16_t freq_buf_idx = inst.inputs[1];
    const std::uint16_t vel_buf_idx  = inst.inputs[2];
    const std::uint16_t trig_buf_idx = inst.inputs[3];
    if (gate_buf_idx == 0xFFFFu || freq_buf_idx == 0xFFFFu ||
        vel_buf_idx == 0xFFFFu || trig_buf_idx == 0xFFFFu) {
        return;  // midi() wasn't bound via `as e`; nothing to fill.
    }
    if (!ctx.buffers) return;

    float* gate_buf = ctx.buffers->get(gate_buf_idx);
    float* freq_buf = ctx.buffers->get(freq_buf_idx);
    float* vel_buf  = ctx.buffers->get(vel_buf_idx);
    float* trig_buf = ctx.buffers->get(trig_buf_idx);
    if (!gate_buf || !freq_buf || !vel_buf || !trig_buf) return;

    std::fill_n(trig_buf, BLOCK_SIZE, 0.0f);

    const double spb_d = (60.0 / static_cast<double>(ctx.bpm))
                       * static_cast<double>(ctx.sample_rate);
    if (!(spb_d > 0.0)) {
        std::fill_n(gate_buf, BLOCK_SIZE, 0.0f);
        std::fill_n(freq_buf, BLOCK_SIZE, 0.0f);
        std::fill_n(vel_buf,  BLOCK_SIZE, 0.0f);
        return;
    }
    const double block_start_b = static_cast<double>(ctx.global_sample_counter) / spb_d;
    const double block_end_b   = block_start_b + (static_cast<double>(BLOCK_SIZE) / spb_d);

    // Collect transitions (note-on / note-off) that fall in the current
    // block window. Capped at 64 transitions/block — well above any
    // realistic MIDI burst (a dense polyphonic stab is ~32 events).
    struct MonoTransition {
        std::uint16_t sample;     // 0..BLOCK_SIZE-1
        bool          is_on;
        std::uint8_t  midi_note;
        float         freq;
        float         velocity;
    };
    constexpr std::size_t MAX_TRANSITIONS = 64;
    MonoTransition transitions[MAX_TRANSITIONS];
    std::size_t num_transitions = 0;

    auto add_transition = [&](double evt_beat, bool is_on,
                              std::uint8_t note, float freq, float vel) {
        if (num_transitions >= MAX_TRANSITIONS) return;
        const double offset_b = evt_beat - block_start_b;
        std::int32_t sample = static_cast<std::int32_t>(offset_b * spb_d);
        if (sample < 0) sample = 0;
        if (sample >= static_cast<std::int32_t>(BLOCK_SIZE)) {
            sample = static_cast<std::int32_t>(BLOCK_SIZE) - 1;
        }
        transitions[num_transitions++] = {
            static_cast<std::uint16_t>(sample), is_on, note, freq, vel
        };
    };

    // HELD_DURATION_SENTINEL is huge; any event with duration above this
    // threshold is "still held" — its note-off has not yet arrived.
    constexpr float SENTINEL_THRESHOLD =
        MidiQueueState::HELD_DURATION_SENTINEL * 0.5f;

    // Snap tolerance for block-boundary comparisons. drain_live_events
    // stores evt.time and the patched duration as floats; reading them
    // back as doubles produces a few-ULP residue against the
    // double-precision block_start_b. The same snap idea POLY uses at
    // vm.cpp:314 — well below sample resolution (1 sample ≈ 4e-5 beats
    // at 110 BPM / 48 kHz) but well above fp noise. A double-fire in the
    // previous block can't happen because evt.duration was sentinel-sized
    // back then and the off-check short-circuited.
    constexpr double BOUNDARY_EPS = 1e-9;

    for (std::uint32_t e = 0; e < s.output.num_events; ++e) {
        const auto& evt = s.output.events[e];
        const float freq = evt.values[0];
        const std::uint8_t note = static_cast<std::uint8_t>(evt.midi_note);

        // note-on transition?
        const double on_b = static_cast<double>(evt.time);
        if (on_b + BOUNDARY_EPS >= block_start_b && on_b < block_end_b) {
            add_transition(on_b, true, note, freq, evt.velocity);
        }

        // note-off transition? (only for events whose note-off has arrived)
        if (evt.duration < SENTINEL_THRESHOLD) {
            const double off_b = on_b + static_cast<double>(evt.duration);
            if (off_b + BOUNDARY_EPS >= block_start_b && off_b < block_end_b) {
                add_transition(off_b, false, note, freq, 0.0f);
            }
        }
    }

    // Insertion-sort transitions by sample (typically < 8 entries).
    for (std::size_t i = 1; i < num_transitions; ++i) {
        for (std::size_t j = i;
             j > 0 && transitions[j].sample < transitions[j - 1].sample;
             --j) {
            std::swap(transitions[j], transitions[j - 1]);
        }
    }

    // Initial output state from the carry-over stack.
    float current_freq = 0.0f;
    float current_vel  = 0.0f;
    float current_gate = 0.0f;
    if (s.mono_stack_depth > 0) {
        const auto& top = s.mono_held_stack[s.mono_stack_depth - 1];
        current_freq = top.freq;
        current_vel  = top.velocity;
        current_gate = 1.0f;
    }

    auto stack_push = [&](std::uint8_t note, float freq, float vel) {
        if (s.mono_stack_depth >= MidiQueueState::MONO_STACK_CAPACITY) {
            // Evict oldest entry to make room.
            for (std::uint8_t k = 0;
                 k + 1 < MidiQueueState::MONO_STACK_CAPACITY; ++k) {
                s.mono_held_stack[k] = s.mono_held_stack[k + 1];
            }
            s.mono_stack_depth = MidiQueueState::MONO_STACK_CAPACITY - 1;
        }
        auto& slot = s.mono_held_stack[s.mono_stack_depth++];
        slot.note     = static_cast<std::int16_t>(note);
        slot.freq     = freq;
        slot.velocity = vel;
    };

    auto stack_remove = [&](std::uint8_t note) {
        int idx = -1;
        for (int k = static_cast<int>(s.mono_stack_depth) - 1; k >= 0; --k) {
            if (s.mono_held_stack[k].note == static_cast<std::int16_t>(note)) {
                idx = k;
                break;
            }
        }
        if (idx < 0) return;
        for (std::uint8_t k = static_cast<std::uint8_t>(idx);
             k + 1 < s.mono_stack_depth; ++k) {
            s.mono_held_stack[k] = s.mono_held_stack[k + 1];
        }
        --s.mono_stack_depth;
    };

    // Walk the block, applying transitions in chronological order.
    std::size_t ti = 0;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        while (ti < num_transitions && transitions[ti].sample == i) {
            const auto& t = transitions[ti];
            if (t.is_on) {
                stack_push(t.midi_note, t.freq, t.velocity);
                trig_buf[i] = 1.0f;
                current_freq = t.freq;
                current_vel  = t.velocity;
                current_gate = 1.0f;
            } else {
                stack_remove(t.midi_note);
                if (s.mono_stack_depth == 0) {
                    current_gate = 0.0f;
                    // Keep current_freq/current_vel unchanged — irrelevant
                    // when gate is 0 and avoids spurious oscillator phase
                    // resets if the downstream osc reads them anyway.
                } else {
                    const auto& top =
                        s.mono_held_stack[s.mono_stack_depth - 1];
                    current_freq = top.freq;
                    current_vel  = top.velocity;
                    current_gate = 1.0f;
                }
            }
            ++ti;
        }
        gate_buf[i] = current_gate;
        freq_buf[i] = current_freq;
        vel_buf[i]  = current_vel;
    }
}

}  // namespace

void op_midi_query(ExecutionContext& ctx, const Instruction& inst) {
    auto& s = ctx.states->get_or_create<MidiQueueState>(inst.state_id);

    drain_live_events_into_output(s, ctx);
    advance_file_seq_into_output(s, ctx);
    fill_mono_buffers(s, ctx, inst);
}

}  // namespace cedar
