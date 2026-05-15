#include "cedar/opcodes/midi.hpp"

#include <algorithm>
#include <cmath>

#include "cedar/dsp/constants.hpp"
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

// Phase 5 will fill this in (parse the .mid play head, advance over the
// block range, emit note-on / note-off events). Phase 1 keeps the call
// site so the opcode dispatch doesn't change shape later.
void advance_file_seq_into_output(MidiQueueState& s,
                                  const ExecutionContext& /*ctx*/) {
    if (!s.file_seq) return;
    // Stubbed until Phase 5.
}

}  // namespace

void op_midi_query(ExecutionContext& ctx, const Instruction& inst) {
    auto& s = ctx.states->get_or_create<MidiQueueState>(inst.state_id);

    drain_live_events_into_output(s, ctx);
    advance_file_seq_into_output(s, ctx);
}

}  // namespace cedar
