#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "sequence.hpp"  // for OutputEvents

namespace cedar {

struct ExecutionContext;
struct Instruction;
struct MidiSequence;  // cedar/io/midi_sequence.hpp — full definition is only
                      // needed by op_midi_query / smf_parser, so keep this
                      // header free of the IO include.

// Source kind selected by the akkado `midi()` builtin. Phase 1 stores the
// value on the state but only the live-device path is wired through
// `op_midi_query`. Phase 2 plugs Akkado codegen into init_midi_queue_state;
// Phase 5 fills in the .mid file path.
enum class MidiSourceKind : std::uint8_t {
    DefaultDevice = 0,
    NamedDevice,
    File,
};

// Raw channel-voice MIDI event as published into the SPSC ring by
// `push_midi_event`. Sample-stamped so that a Phase-N sample-accurate
// scheduler can refine the block-boundary timing this phase uses; Phase 1
// drains the whole ring at the start of each block.
struct MidiRawEvent {
    std::uint64_t sample_ts = 0;
    std::uint8_t  status    = 0;
    std::uint8_t  d1        = 0;
    std::uint8_t  d2        = 0;
    std::uint8_t  channel   = 0;
};

// State for one `midi()` source. Holds the SPSC ring, the
// note-to-event-index map used to patch durations on note-off, and the
// persistent OutputEvents the POLY block reads each block.
//
// The ring is arena-allocated (NOT inline) so std::variant<DSPState> stays
// small — an inline 1024-event ring would cost ~16 KB per state slot and
// blow up the StatePool by tens of megabytes.
struct MidiQueueState {
    enum TempoMode : std::uint8_t { Follow = 0, File = 1 };

    // Held-note duration sentinel. Must be much larger than any block_end_pos
    // POLY can see during a realistic session so the gate-off check on a
    // still-held event evaluates to false in every block until note-off
    // arrives and patches `duration`. Paired with `cycle_length = 1e6f` below
    // (which keeps `current_cycle == 0` and disables POLY's cycle-wrap
    // branch), this lets MIDI events use absolute beat coordinates the same
    // shape SEQPAT_QUERY produces.
    static constexpr float HELD_DURATION_SENTINEL = 1.0e9f;

    // Default ring capacity in events. Far above bursts a realistic MIDI
    // source produces (a 16-voice polyphonic stab is ~32 events; a dense
    // synth pad sequence ~50/sec).
    static constexpr std::uint32_t DEFAULT_RING_CAPACITY = 1024u;

    // Default OutputEvents capacity. Held events stay in `output` across
    // blocks (see op_midi_query); 4096 entries covers ~hours of normal play.
    static constexpr std::uint32_t DEFAULT_OUTPUT_CAPACITY = 4096u;

    // PRD §7.4: default pending-CC scratch buffer capacity. The audio thread
    // appends here in advance_file_cc_into_output; the host drains after
    // each process_block. 256 entries comfortably covers any realistic
    // per-block CC density (~16-CC sweep per block at 48 kHz / 128 samples).
    static constexpr std::uint32_t DEFAULT_PENDING_CC_CAPACITY = 256u;

    // SPSC ring — single producer (host MIDI callback / test thread) writes
    // through `push_midi_event`; single consumer (audio thread) drains in
    // `op_midi_query`.
    MidiRawEvent* ring = nullptr;
    std::uint32_t ring_capacity = 0;
    std::atomic<std::uint64_t> write_pos{0};
    std::atomic<std::uint64_t> read_pos{0};

    // Source metadata populated by VM::init_midi_queue_state. `file_seq` is
    // set when the host registers parsed bytes via VM::load_midi_file and
    // the registry lookup matches `name_or_path`; nullptr keeps the file
    // path silent and is a documented edge case (loading-in-progress, file
    // not yet dropped, parse failure).
    MidiSourceKind kind          = MidiSourceKind::DefaultDevice;
    std::uint8_t   channel_filter = 0;
    bool           loop           = false;
    TempoMode      tempo_mode     = Follow;
    MidiSequence*  file_seq       = nullptr;
    double         file_play_head_beats = 0.0;
    // Reserved for a future tempo-map cursor optimisation; v1 uses the
    // per-note `beat_on_file`/`beat_off_file` precomputed in parse_smf.
    std::uint32_t  current_tempo_idx     = 0;

    // Note → index of the OutputEvent in `output.events` currently emitting
    // this note. -1 means not held. Used to patch duration on note-off,
    // including across blocks. Initialized in the default ctor so a
    // freshly-emplaced state in StatePool is immediately usable.
    std::int32_t held_note_to_event[128];

    // Persistent across blocks: POLY's release path needs the original
    // note-on event to remain in `output` until note-off arrives and
    // patches its duration. POLY's pre-existing gate-on/gate-off range
    // checks at vm.cpp:355-433 gracefully skip events whose start or end
    // is outside the current block, so leaving past events around costs
    // only the per-block scan time. The buffer is sized generously
    // (DEFAULT_OUTPUT_CAPACITY) and treated as a high-water buffer in
    // Phase 1; compaction is a Phase-N follow-up that requires remapping
    // active voices' event_index fields.
    OutputEvents output;

    // Sentinel-large cycle so `fmod(beat_start, cycle_length) == beat_start`
    // and `current_cycle == 0` for the entire realistic session lifetime.
    // POLY's wrap branch (vm.cpp:401-424) is consequently never triggered,
    // and events use absolute beat coordinates. See PRD note in midi.cpp.
    float cycle_length = 1.0e6f;

    // Producer-side counter: incremented when the ring is full and a new
    // event is dropped (drop-newest policy; the PRD §7 mentions drop-oldest
    // but that breaks strict SPSC by forcing the producer to advance
    // `read_pos`. Phase-N can revisit if it matters).
    std::uint64_t midi_overflow_count = 0;

    // PRD prd-midi-input §7.5: monophonic per-block buffer indices.
    // Allocated by codegen and carried on the MIDI_QUERY instruction
    // (inst.inputs[0..3]); op_midi_query reads them to fill mono-baked
    // gate / freq / vel / trig signals so `midi() as e |> osc(..., e.freq)`
    // works the same way pattern field access does. 0xFFFF (BUFFER_UNUSED)
    // skips the mono-buffer fill step entirely (downstream is poly/sf2 only).
    // Note: indices live on the instruction rather than on the state so the
    // codegen has the freedom to re-allocate buffers on every hot-swap.

    // §7.5: last-note-wins mono priority stack. Each entry remembers
    // freq/velocity so popping back to a fallback note doesn't require
    // re-walking OutputEvents. Size 16 covers any realistic single-hand
    // chord; overflowing pushes drop the oldest entry (FIFO eviction).
    struct MonoHeldNote {
        std::int16_t note     = -1;
        float        freq     = 0.0f;
        float        velocity = 0.0f;
    };
    static constexpr std::uint8_t MONO_STACK_CAPACITY = 16;
    MonoHeldNote mono_held_stack[MONO_STACK_CAPACITY];
    std::uint8_t mono_stack_depth = 0;

    // PRD §7.4: scratch buffer for CC/PB/AT events the file scan unpacks
    // into during the current block. The host drains via
    // `VM::drain_pending_file_ccs` after `process_block` and dispatches each
    // entry through the same route table the live-device path uses.
    //
    // Field layout matches the (status, d1, d2, channel) tuple
    // `dispatch_midi_cc` reads — that lets the drain call into the existing
    // dispatcher without re-encoding. `channel` is 0..15 (zero-indexed);
    // the dispatcher adds +1 itself when matching against MidiCcRoute.
    struct PendingCc {
        std::uint8_t status  = 0;
        std::uint8_t d1      = 0;
        std::uint8_t d2      = 0;
        std::uint8_t channel = 0;
    };
    PendingCc*    pending_ccs          = nullptr;
    std::uint32_t pending_cc_capacity  = 0;
    std::uint32_t num_pending_ccs      = 0;
    std::uint64_t file_cc_overflow_count = 0;

    // PRD prd-midi-input §7.3: hot-swap held-note migration. Set by
    // `VM::init_midi_queue_state` when a re-init detects this state already
    // has arena-backed storage AND still has held notes. `op_midi_query`
    // checks the flag at the start of the next block, walks
    // `held_note_to_event[]`, and re-emits synthetic note-ons at the new
    // block's beat position so a downstream POLY/SF re-allocates voices
    // sample-accurately rather than restarting silently.
    bool pending_held_note_remigration = false;

    // PRD §7.3: last name_or_path passed to init_midi_queue_state. The
    // file-mode play head is only preserved across re-init when the new
    // call's URI matches this one; otherwise the head resets to 0. Held in
    // a std::string (heap-allocated; the host thread runs the init path).
    std::string last_file_uri;

    [[nodiscard]] bool has_any_held_notes() const noexcept {
        for (std::int32_t v : held_note_to_event) {
            if (v >= 0) return true;
        }
        return false;
    }

    // PRD §7.3 GC-sweep fallback. Walk `held_note_to_event[]` and patch each
    // matching event's `duration` so its note-off lands at `now_beats`. Used
    // when a `midi()` call disappears entirely from the program: downstream
    // consumers that the hot-swap preserved see finite durations and run
    // their normal note-off path instead of stuck-voice sentinel readings.
    // Single-sample-floor mirrors the same-block patch in
    // drain_live_events_into_output so POLY/SF still fire the gate-off when
    // the migration runs at the exact block boundary.
    void release_all_held(float now_beats, float samples_per_beat) noexcept {
        constexpr float SENTINEL_THRESHOLD = HELD_DURATION_SENTINEL * 0.5f;
        for (std::int32_t& idx_ref : held_note_to_event) {
            const std::int32_t idx = idx_ref;
            if (idx < 0 ||
                static_cast<std::uint32_t>(idx) >= output.num_events) {
                idx_ref = -1;
                continue;
            }
            auto& evt = output.events[idx];
            if (evt.duration < SENTINEL_THRESHOLD) {
                idx_ref = -1;
                continue;
            }
            float dur = now_beats - evt.time;
            if (!(dur > 0.0f)) {
                dur = (samples_per_beat > 0.0f) ? (1.0f / samples_per_beat)
                                                : 1.0e-6f;
            }
            evt.duration = dur;
            idx_ref = -1;
        }
    }

    MidiQueueState() {
        for (std::int32_t& v : held_note_to_event) v = -1;
    }

    // std::atomic isn't copy/move-constructible, so the variant's implicit
    // move assignment (used by StatePool::gc_sweep and clear_all) gets
    // deleted unless we provide explicit move ops. We hand-roll moves that
    // read each atomic via load() and reconstruct in-place. Copies are
    // disallowed — the SPSC contract assumes a single producer/consumer
    // pair pinned to one instance.
    MidiQueueState(const MidiQueueState&) = delete;
    MidiQueueState& operator=(const MidiQueueState&) = delete;

    MidiQueueState(MidiQueueState&& other) noexcept
        : ring(other.ring),
          ring_capacity(other.ring_capacity),
          write_pos(other.write_pos.load(std::memory_order_relaxed)),
          read_pos(other.read_pos.load(std::memory_order_relaxed)),
          kind(other.kind),
          channel_filter(other.channel_filter),
          loop(other.loop),
          tempo_mode(other.tempo_mode),
          file_seq(other.file_seq),
          file_play_head_beats(other.file_play_head_beats),
          current_tempo_idx(other.current_tempo_idx),
          output(other.output),
          cycle_length(other.cycle_length),
          midi_overflow_count(other.midi_overflow_count),
          mono_stack_depth(other.mono_stack_depth),
          pending_ccs(other.pending_ccs),
          pending_cc_capacity(other.pending_cc_capacity),
          num_pending_ccs(other.num_pending_ccs),
          file_cc_overflow_count(other.file_cc_overflow_count),
          pending_held_note_remigration(other.pending_held_note_remigration),
          last_file_uri(std::move(other.last_file_uri))
    {
        for (std::size_t i = 0; i < 128; ++i) {
            held_note_to_event[i] = other.held_note_to_event[i];
        }
        for (std::size_t i = 0; i < MONO_STACK_CAPACITY; ++i) {
            mono_held_stack[i] = other.mono_held_stack[i];
        }
    }

    MidiQueueState& operator=(MidiQueueState&& other) noexcept {
        if (this != &other) {
            ring               = other.ring;
            ring_capacity      = other.ring_capacity;
            write_pos.store(other.write_pos.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
            read_pos.store(other.read_pos.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
            kind                 = other.kind;
            channel_filter       = other.channel_filter;
            loop                 = other.loop;
            tempo_mode           = other.tempo_mode;
            file_seq             = other.file_seq;
            file_play_head_beats = other.file_play_head_beats;
            current_tempo_idx    = other.current_tempo_idx;
            output               = other.output;
            cycle_length         = other.cycle_length;
            midi_overflow_count  = other.midi_overflow_count;
            mono_stack_depth     = other.mono_stack_depth;
            pending_ccs           = other.pending_ccs;
            pending_cc_capacity   = other.pending_cc_capacity;
            num_pending_ccs       = other.num_pending_ccs;
            file_cc_overflow_count = other.file_cc_overflow_count;
            pending_held_note_remigration = other.pending_held_note_remigration;
            last_file_uri        = std::move(other.last_file_uri);
            for (std::size_t i = 0; i < 128; ++i) {
                held_note_to_event[i] = other.held_note_to_event[i];
            }
            for (std::size_t i = 0; i < MONO_STACK_CAPACITY; ++i) {
                mono_held_stack[i] = other.mono_held_stack[i];
            }
        }
        return *this;
    }
};

static_assert(sizeof(MidiQueueState) < 1024,
              "MidiQueueState must stay small to keep std::variant<DSPState> "
              "bounded; arena-allocate any large data instead.");

// Block-start drain. Reads up to (write_pos - read_pos) events from the SPSC
// ring, emits note-on as a fresh OutputEvent with HELD_DURATION_SENTINEL,
// and patches the matching note-on's duration on note-off. Other MIDI
// status bytes (CC, pitch-bend, channel pressure, sysex, …) are silently
// discarded in Phase 1 — `midi_cc` routing lands in Phase 6.
void op_midi_query(ExecutionContext& ctx, const Instruction& inst);

}  // namespace cedar
