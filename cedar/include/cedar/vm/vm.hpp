#pragma once

#include "instruction.hpp"
#include "context.hpp"
#include "buffer_pool.hpp"
#include "state_pool.hpp"
#include "env_map.hpp"
#include "sample_bank.hpp"
#ifndef CEDAR_NO_SOUNDFONT
#include "../audio/soundfont.hpp"
#endif
#ifndef CEDAR_NO_FFT
#include "../wavetable/registry.hpp"
#endif
#include "swap_controller.hpp"
#include "crossfade_state.hpp"
#include "../opcodes/dsp_state.hpp"
#include "../opcodes/midi.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cedar {

// Configuration for seek operations
struct SeekConfig {
    bool reset_history_dependent = true;   // Reset filters/delays to zero state
    std::uint32_t preroll_blocks = 0;      // Number of blocks to process silently after seek
};

// Register-based bytecode VM for audio processing
// Processes entire blocks (128 samples) at a time for cache efficiency
// Supports glitch-free hot-swapping with crossfade for live coding
class VM {
public:
    // Result of loading a program
    enum class LoadResult {
        Success,            // Program queued for swap
        SlotBusy,          // No write slot available (should never happen)
        InvalidProgram,    // Program validation failed
        TooLarge           // Program exceeds MAX_PROGRAM_SIZE
    };

    VM();
    ~VM();

    // Non-copyable (owns buffer pool and state pool)
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;
    VM(VM&&) noexcept = default;
    VM& operator=(VM&&) noexcept = default;

    // =========================================================================
    // Program Loading (Thread-safe - can be called from any thread)
    // =========================================================================

    // Load new program for hot-swap
    // This is the primary API for live coding updates
    // Returns immediately - actual swap happens at next block boundary
    [[nodiscard]] LoadResult load_program(std::span<const Instruction> bytecode);

    // Force immediate program load (resets all state)
    // Only use for initial load, not during playback
    bool load_program_immediate(std::span<const Instruction> bytecode);

    // Stage a FOREACH_EVENT subprogram table for the NEXT load_program() /
    // load_program_immediate() call (PRD prd-runtime-functions-control-flow L3).
    // The staged table is consumed (and cleared) by that load. Call this
    // immediately before load_program* when the compiled program carries
    // FOREACH_EVENT blocks; programs with no blocks need not call it.
    // `bytecode` passed to the subsequent load is the full [ main | bodies ]
    // stream; `main_inst_count` is the main/body boundary.
    void set_block_table(std::span<const BlockEntry> block_table,
                         std::uint32_t main_inst_count);

    // Load a program that carries a FOREACH_EVENT subprogram table (PRD L3).
    // Convenience: stages the table, then loads. `bytecode` is the full
    // [ main | block bodies ] stream.
    [[nodiscard]] LoadResult load_program_with_blocks(
        std::span<const Instruction> bytecode,
        std::span<const BlockEntry> block_table,
        std::uint32_t main_inst_count);

    // Immediate variant of load_program_with_blocks (initial load only).
    bool load_program_with_blocks_immediate(
        std::span<const Instruction> bytecode,
        std::span<const BlockEntry> block_table,
        std::uint32_t main_inst_count);

    // =========================================================================
    // Audio Processing (Audio thread only)
    // =========================================================================

    // Process one block of audio (128 samples)
    // Handles swap and crossfade automatically at block boundaries
    void process_block(float* output_left, float* output_right);

    // Set input buffer pointers used by the INPUT opcode.
    // The host populates these buffers each block before calling process_block.
    // Pass nullptr to indicate "no input available" — INPUT then writes silence.
    // The pointers persist across blocks until changed.
    void set_input_buffers(float* input_left, float* input_right);

    // =========================================================================
    // State Management
    // =========================================================================

    // Full reset (clear all state, stop any crossfade)
    void reset();

    // Legacy hot-swap API (for backwards compatibility)
    void hot_swap_begin();
    void hot_swap_end();

    // Configure crossfade duration (2-5 blocks, default 3)
    void set_crossfade_blocks(std::uint32_t blocks);

    // =========================================================================
    // Timeline Seek (for DAW/VST integration)
    // =========================================================================

    // Seek to a specific beat position
    // Reconstructs deterministic state (oscillator phases, LFO phases, etc.)
    // Optionally resets history-dependent state (filters, delays) and runs pre-roll
    void seek(float beat_position, const SeekConfig& config = {});

    // Seek to a specific sample position
    void seek_samples(std::uint64_t sample_position, const SeekConfig& config = {});

    // Query current position
    [[nodiscard]] float current_beat_position() const;
    [[nodiscard]] std::uint64_t current_sample_position() const;

    // =========================================================================
    // Configuration
    // =========================================================================

    void set_sample_rate(float rate);
    void set_bpm(float bpm);

    // =========================================================================
    // External Parameter Binding (Thread-safe - can be called from any thread)
    // =========================================================================

    // Set external parameter value (creates if doesn't exist)
    // Returns false if MAX_ENV_PARAMS reached
    bool set_param(const char* name, float value);

    // Set parameter with custom slew time in milliseconds
    bool set_param(const char* name, float value, float slew_ms);

    // Remove external parameter
    void remove_param(const char* name);

    // Check if external parameter exists
    [[nodiscard]] bool has_param(const char* name) const;

    // =========================================================================
    // Sample Management
    // =========================================================================

    // Load a sample into the sample bank
    // Returns sample ID, or 0 if loading failed
    std::uint32_t load_sample(const std::string& name,
                              const float* audio_data,
                              std::size_t num_samples,
                              std::uint32_t channels,
                              float sample_rate);

    // Get sample bank (for direct access)
    [[nodiscard]] SampleBank& sample_bank() { return sample_bank_; }
    [[nodiscard]] const SampleBank& sample_bank() const { return sample_bank_; }

#ifndef CEDAR_NO_SOUNDFONT
    // Get SoundFont registry (for SF2 management)
    [[nodiscard]] SoundFontRegistry& soundfont_registry() { return soundfont_registry_; }
    [[nodiscard]] const SoundFontRegistry& soundfont_registry() const { return soundfont_registry_; }
#endif

#ifndef CEDAR_NO_FFT
    // Get wavetable registry (for wt_load / smooch).
    [[nodiscard]] WavetableBankRegistry& wavetable_registry() { return wavetable_registry_; }
    [[nodiscard]] const WavetableBankRegistry& wavetable_registry() const { return wavetable_registry_; }
#endif

    // Initialize a SequenceState with compiled sequences (arena-allocated)
    // Used by compiler to set up the simplified sequence-based patterns
    // @param total_events Total event count across all sequences (for output buffer sizing)
    void init_sequence_program_state(std::uint32_t state_id,
                                     const Sequence* sequences, std::size_t seq_count,
                                     float cycle_length, bool is_sample_pattern,
                                     std::uint32_t total_events) {
        state_pool_.init_sequence_program(state_id, sequences, seq_count,
                                          cycle_length, is_sample_pattern,
                                          &audio_arena_, total_events);
    }

    // PRD prd-runtime-event-transforms Phase 4 Commit C: legacy
    // init_sequence_iter_state was removed alongside SequenceState's
    // iter_n / iter_dir fields. iter() / iterBack() now lower to
    // EVENT_REORDER(ITER) and use the standard init_event_transform_state
    // path. Hosts must not call init_sequence_iter_state — the symbol no
    // longer exists.

    // Initialize a transform-owned SequenceState for an EVENT_MAP / EVENT_FILTER
    // opcode (PRD prd-runtime-event-transforms, Phase 1). Allocates only the
    // OutputEvents buffer — the opcode fills it at runtime from its upstream.
    void init_event_transform_state(std::uint32_t state_id, float cycle_length,
                                    bool is_sample_pattern,
                                    std::uint32_t output_capacity) {
        state_pool_.init_event_transform(state_id, cycle_length,
                                         is_sample_pattern, &audio_arena_,
                                         output_capacity);
    }

    // Initialize a RateScaleState for an EVENT_RATE_SCALE opcode (PRD
    // prd-runtime-event-transforms Phase 3). No buffers — the integrator
    // state is the entire payload. Resets accumulated beat_pos to 0.
    void init_event_rate_scale_state(std::uint32_t state_id) {
        state_pool_.init_event_rate_scale(state_id);
    }

    // Initialize ExtendedParams<N> state for an opcode that uses more
    // than 5 input slots. The loader / WASM apply-state-inits path calls
    // this with a runtime-known count; the StatePool helper picks the
    // smallest variant that fits. See docs/extended-params-mechanism.md.
    void init_extended_params(std::uint32_t state_id,
                              const float* constants,
                              const std::uint16_t* buffer_indices,
                              std::uint8_t count) {
        state_pool_.init_extended_params_runtime(state_id, constants,
                                                 buffer_indices, count);
    }

    // Initialize polyphony state for a POLY_BEGIN opcode.
    //
    // @param release_seconds  Per-instance release window in seconds. 0
    //   (default) preserves the legacy "gate-multiplied silence at
    //   note-off" behavior. > 0 holds the mix-side gate at 1.0 for the
    //   first `release_seconds * ctx_.sample_rate` samples after note-off
    //   so the voice's own ADSR can run its release tail. See PRD
    //   prd-midi-input §7.2.
    // Copy the Phase 3 custom-property config onto a PolyAllocState. Shared
    // by init_poly_state and the VOICE_POOL branch of init_foreach_state.
    static void apply_poly_props(PolyAllocState& state, std::uint8_t prop_count,
                                 const float* prop_defaults) {
        state.prop_count = std::min(
            prop_count, static_cast<std::uint8_t>(MAX_PROPS_PER_EVENT));
        for (std::size_t i = 0; i < MAX_PROPS_PER_EVENT; ++i) {
            state.prop_defaults[i] = prop_defaults ? prop_defaults[i] : 0.0f;
        }
    }

    //
    // @param prop_count     Number of custom record-suffix property slots
    //   plumbed into the per-voice field bank after the 11 fixed fields
    //   (Phase 3, prd-poly-callback-event-record). 0 = no custom fields.
    // @param prop_defaults  Per-slot callback destructure defaults, applied
    //   when a voice's event omits the property. May be null (all 0.0).
    void init_poly_state(std::uint32_t state_id, std::uint32_t seq_state_id,
                         std::uint8_t max_voices, std::uint8_t mode,
                         std::uint8_t steal_strategy,
                         float release_seconds = 0.0f,
                         std::uint8_t prop_count = 0,
                         const float* prop_defaults = nullptr) {
        auto& state = state_pool_.get_or_create<PolyAllocState>(state_id);
        state.seq_state_id = seq_state_id;
        state.max_voices = std::min(max_voices, static_cast<std::uint8_t>(PolyAllocState::MAX_VOICES));
        state.mode = mode;
        state.steal_strategy = steal_strategy;
        const float release_clamped = release_seconds > 0.0f ? release_seconds : 0.0f;
        state.release_window_samples = release_clamped * ctx_.sample_rate;
        apply_poly_props(state, prop_count, prop_defaults);
        state.ensure_voices(&audio_arena_);
    }

    // Initialize FOREACH_EVENT instance state (PRD prd-runtime-functions-control-flow L3).
    // `allocator_kind` selects the state struct: 0=VOICE_POOL (PolyAllocState),
    // 1=PER_ITERATION (ForeachIterState), 2=SHARED (ForeachSharedState).
    // For VOICE_POOL the voice config (max_voices/mode/steal/release) is reused
    // exactly as init_poly_state — POLY migrates onto this path bit-exact.
    void init_foreach_state(std::uint32_t state_id, std::uint8_t allocator_kind,
                            std::uint32_t block_id,
                            std::uint32_t event_src_state_id,
                            std::uint16_t max_iterations,
                            std::uint8_t max_voices, std::uint8_t mode,
                            std::uint8_t steal_strategy,
                            float release_seconds = 0.0f,
                            std::uint8_t prop_count = 0,
                            const float* prop_defaults = nullptr) {
        switch (allocator_kind) {
            case 0: {  // VOICE_POOL — identical setup to init_poly_state
                auto& state = state_pool_.get_or_create<PolyAllocState>(state_id);
                state.seq_state_id = event_src_state_id;
                state.block_id = block_id;
                state.max_voices = std::min(
                    max_voices,
                    static_cast<std::uint8_t>(PolyAllocState::MAX_VOICES));
                state.mode = mode;
                state.steal_strategy = steal_strategy;
                const float rc = release_seconds > 0.0f ? release_seconds : 0.0f;
                state.release_window_samples = rc * ctx_.sample_rate;
                apply_poly_props(state, prop_count, prop_defaults);
                state.ensure_voices(&audio_arena_);
                break;
            }
            case 1: {  // PER_ITERATION
                auto& state = state_pool_.get_or_create<ForeachIterState>(state_id);
                state.block_id = block_id;
                state.seq_state_id = event_src_state_id;
                state.max_iterations = max_iterations;
                break;
            }
            case 2: {  // SHARED
                auto& state = state_pool_.get_or_create<ForeachSharedState>(state_id);
                state.block_id = block_id;
                state.seq_state_id = event_src_state_id;
                break;
            }
            default:
                break;
        }
    }

    // PRD prd-midi-input §7.1: initialize a SoundFontVoiceState for the
    // event-driven path. Codegen pushes one of these whenever the upstream
    // is a runtime event source (midi() — and any future producer that
    // sets is_runtime_event_source on its PatternPayload). The SF2 voice
    // opcode then resolves events via seq_state_id instead of reading the
    // legacy gate/freq/vel buffers from instruction inputs.
#ifndef CEDAR_NO_SOUNDFONT
    void init_soundfont_voice_event_state(std::uint32_t state_id,
                                          std::uint32_t seq_state_id,
                                          int preset_idx) {
        auto& state = state_pool_.get_or_create<SoundFontVoiceState>(state_id);
        state.has_upstream_events = true;
        state.seq_state_id        = seq_state_id;
        state.preset_idx          = preset_idx;
        state.ensure_voices(&audio_arena_);
    }
#endif

    // Initialize a MidiQueueState for a MIDI_QUERY opcode.
    //
    // Allocates the SPSC ring (DEFAULT_RING_CAPACITY events) and the
    // OutputEvents buffer (DEFAULT_OUTPUT_CAPACITY entries) from the audio
    // arena on first call. Idempotent on hot-swap: when the same state_id
    // already has arena-allocated storage, the existing buffers are reused
    // and the metadata fields are updated in place. Live events queued on
    // the previous program survive — held notes will emit synthetic
    // note-offs through the Phase-2 hot-swap path (PRD §4.12).
    //
    // Phase 1: only `channel_filter` and the device-kind metadata are used
    // — file_seq stays nullptr (.mid playback ships in Phase 5), loop /
    // tempo_mode are stored for forward-compat.
    //
    // @param state_id        FNV-1a hash for this midi() call site
    // @param kind            DefaultDevice / NamedDevice / File
    // @param name_or_path    Device substring or file URI (Phase 1 stores
    //                        for diagnostics only). May be nullptr.
    // @param channel_filter  0 = any, 1-16 = match incoming MIDI channel
    // @param loop            File-mode loop flag (Phase 5)
    // @param tempo           File-mode tempo policy (Phase 5)
    void init_midi_queue_state(std::uint32_t state_id,
                               MidiSourceKind kind,
                               const char* name_or_path,
                               std::uint8_t channel_filter,
                               bool loop,
                               MidiQueueState::TempoMode tempo) {
        auto& s = state_pool_.get_or_create<MidiQueueState>(state_id);

        // PRD §7.3 hot-swap held-note migration: a re-init while arena-backed
        // storage exists means this state survived a hot-swap. Capture the
        // signal BEFORE overwriting any field so the next op_midi_query can
        // re-emit synthetic note-ons at the new block's beat position.
        const bool was_reused = (s.ring != nullptr);
        std::string new_uri = (name_or_path && *name_or_path)
                                  ? std::string(name_or_path)
                                  : std::string();

        s.kind           = kind;
        s.channel_filter = channel_filter;
        s.loop           = loop;
        s.tempo_mode     = tempo;

        // File-kind sources look up their parsed sequence in the registry the
        // host populates via load_midi_file(). On hot-swap with a re-init
        // before bytes land we leave file_seq nullptr — the opcode is silent
        // until the host pushes bytes, which is the documented edge case
        // (PRD §7 "`.mid` not yet loaded at compile").
        if (kind == MidiSourceKind::File && !new_uri.empty()) {
            const auto it = midi_sequences_.find(new_uri);
            s.file_seq = (it != midi_sequences_.end()) ? it->second : nullptr;
            // Preserve play-head only when the new init matches the file URI
            // that produced the current position; otherwise reset so a new
            // file starts from beat 0.
            if (!was_reused || s.last_file_uri != new_uri) {
                s.file_play_head_beats = 0.0;
                s.current_tempo_idx = 0;
            }
        } else {
            s.file_seq = nullptr;
            if (!was_reused) {
                s.file_play_head_beats = 0.0;
                s.current_tempo_idx = 0;
            }
        }

        // Stash the URI for the next re-init comparison.
        s.last_file_uri = std::move(new_uri);

        // Schedule held-note migration if a hot-swap left notes still
        // sounding. op_midi_query consumes the flag at the start of its
        // next call, BEFORE the live-event drain so a note-off arriving in
        // the same block patches the right (newly synthesized) event index.
        if (was_reused) {
            s.pending_held_note_remigration = s.has_any_held_notes();
        }

        // Arena-allocate the ring on first init only; reuse on hot-swap to
        // avoid clobbering the producer cursor (a Phase-N improvement could
        // resize when capacity changes, but Phase 1 keeps it fixed).
        if (!s.ring || s.ring_capacity == 0) {
            const std::uint32_t cap = MidiQueueState::DEFAULT_RING_CAPACITY;
            const std::size_t bytes = static_cast<std::size_t>(cap) * sizeof(MidiRawEvent);
            const std::size_t floats = (bytes + sizeof(float) - 1) / sizeof(float);
            float* mem = audio_arena_.allocate(floats);
            if (mem) {
                s.ring = reinterpret_cast<MidiRawEvent*>(mem);
                s.ring_capacity = cap;
                // Zero-init for deterministic playback even before first push.
                for (std::uint32_t i = 0; i < cap; ++i) {
                    s.ring[i] = MidiRawEvent{};
                }
                s.write_pos.store(0, std::memory_order_relaxed);
                s.read_pos.store(0, std::memory_order_relaxed);
            }
        }

        // Same for the OutputEvents buffer.
        if (!s.output.events || s.output.capacity == 0) {
            const std::uint32_t cap = MidiQueueState::DEFAULT_OUTPUT_CAPACITY;
            const std::size_t bytes = static_cast<std::size_t>(cap) * sizeof(OutputEvents::OutputEvent);
            const std::size_t floats = (bytes + sizeof(float) - 1) / sizeof(float);
            float* mem = audio_arena_.allocate(floats);
            if (mem) {
                s.output.events = reinterpret_cast<OutputEvents::OutputEvent*>(mem);
                s.output.capacity = cap;
                s.output.num_events = 0;
            }
        }

        // PRD §7.4 file-CC scratch. Same allocation pattern as ring/output:
        // first init wins, reuse on hot-swap.
        if (!s.pending_ccs || s.pending_cc_capacity == 0) {
            const std::uint32_t cap =
                MidiQueueState::DEFAULT_PENDING_CC_CAPACITY;
            const std::size_t bytes = static_cast<std::size_t>(cap)
                                    * sizeof(MidiQueueState::PendingCc);
            const std::size_t floats =
                (bytes + sizeof(float) - 1) / sizeof(float);
            float* mem = audio_arena_.allocate(floats);
            if (mem) {
                s.pending_ccs =
                    reinterpret_cast<MidiQueueState::PendingCc*>(mem);
                s.pending_cc_capacity = cap;
                s.num_pending_ccs = 0;
            }
        }
    }

    // PRD prd-midi-input §7.4: drain the pending file-CC scratch buffer for
    // the addressed `midi()` state into the supplied callback. Resets the
    // count so subsequent process_block calls start with a clean buffer.
    // The callback is invoked once per event with the (status, d1, d2,
    // channel) tuple the host's `dispatch_midi_cc` expects.
    //
    // Safe to call from the audio thread *after* `process_block` has
    // completed; the buffer is single-producer (op_midi_query writes) and
    // single-consumer (this drain). Channel filter is already applied at
    // append-time.
    //
    // @return number of events drained (0 if state not found / kind != File).
    template<typename F>
    std::uint32_t drain_pending_file_ccs(std::uint32_t state_id, F&& cb) {
        auto* s = state_pool_.get_if<MidiQueueState>(state_id);
        if (!s || !s->pending_ccs || s->num_pending_ccs == 0) return 0;
        const std::uint32_t n = s->num_pending_ccs;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& e = s->pending_ccs[i];
            cb(e.status, e.d1, e.d2, e.channel);
        }
        s->num_pending_ccs = 0;
        return n;
    }

    // Parse a `.mid` byte buffer and stash the resulting MidiSequence in
    // the VM's name-keyed registry. Idempotent: a second call with the same
    // name short-circuits and returns the cached pointer's status. Mirrors
    // the soundfont-load shape so the asset pipeline in both web and CLI
    // treats `.mid` files the same as other binary assets.
    //
    // Side effect: walks the state pool and attaches the parsed sequence to
    // any MidiQueueState whose kind == File and whose stored name matches.
    // This makes load-after-init work without forcing the host to call
    // init_midi_queue_state in a particular order relative to load.
    //
    // @return 0 on success, negative on parse failure (file rejected or
    //         arena exhausted). Failed entries are not cached.
    std::int32_t load_midi_file(std::string_view name,
                                const std::uint8_t* bytes,
                                std::size_t len);

    // Free the parsed MidiSequence registry. Pointers themselves live in
    // audio_arena_ and become invalid the moment that arena resets; the
    // map is cleared so a later load_midi_file does not return a dangling
    // pointer. Called from the VM's program-reset paths.
    void clear_midi_sequences();

    // Push a raw channel-voice MIDI event into the queue for the given
    // state_id. Thread-safe single-producer enqueue. The VM stamps the
    // event with its current sample counter so a Phase-N sample-accurate
    // scheduler can refine block-boundary timing; Phase 1 drains the whole
    // queue at the start of each block and emits everything at the block's
    // beat position.
    //
    // Drop-newest on a full ring: increments midi_overflow_count and
    // discards the new event. Strict SPSC: only this method (and the
    // hosts wired to it in Phases 3-4) ever touches write_pos.
    //
    // @return true if the event was enqueued, false if dropped due to a
    //         missing state or a full ring.
    bool push_midi_event(std::uint32_t state_id,
                         std::uint8_t status,
                         std::uint8_t d1,
                         std::uint8_t d2) {
        auto* s = state_pool_.get_if<MidiQueueState>(state_id);
        if (!s || !s->ring || s->ring_capacity == 0) return false;

        const std::uint64_t w = s->write_pos.load(std::memory_order_relaxed);
        const std::uint64_t r = s->read_pos.load(std::memory_order_acquire);
        if (w - r >= s->ring_capacity) {
            ++s->midi_overflow_count;
            return false;
        }

        MidiRawEvent& slot = s->ring[w % s->ring_capacity];
        slot.sample_ts = current_sample_position();
        slot.status    = status;
        slot.d1        = d1;
        slot.d2        = d2;
        slot.channel   = static_cast<std::uint8_t>(status & 0x0Fu);

        // Release publishes the payload writes above.
        s->write_pos.store(w + 1, std::memory_order_release);
        return true;
    }

    // =========================================================================
    // Query API
    // =========================================================================

    [[nodiscard]] bool is_crossfading() const;
    [[nodiscard]] float crossfade_position() const;
    [[nodiscard]] bool has_program() const;
    [[nodiscard]] std::uint32_t swap_count() const;

    // Debug/diagnostic methods
    [[nodiscard]] bool has_pending_swap() const;
    [[nodiscard]] std::uint32_t current_slot_instruction_count() const;
    [[nodiscard]] std::uint32_t previous_slot_instruction_count() const;

    // Accessors (for testing/debugging)
    [[nodiscard]] const ExecutionContext& context() const { return ctx_; }
    [[nodiscard]] BufferPool& buffers() { return buffer_pool_; }
    [[nodiscard]] StatePool& states() { return state_pool_; }
    [[nodiscard]] EnvMap& env_map() { return env_map_; }

private:
    // Execute program from a specific slot
    void execute_program(const ProgramSlot* slot, float* out_left, float* out_right);

    // Execute single instruction
    void execute(const Instruction& inst);

    // Execute a legacy POLY block (POLY_BEGIN/POLY_END inline body) — iterates
    // voices, sets XOR isolation, accumulates mix. Returns the instruction
    // pointer past POLY_END. Retained for the --legacy-poly compat window.
    std::size_t execute_poly_block(std::span<const Instruction> program, std::size_t ip);

    // Execute a FOREACH_EVENT opcode (PRD prd-runtime-functions-control-flow L3).
    // Dispatches the subprogram body from slot->blocks[] once per event/voice
    // per the allocator kind. The body lives in the subprogram table, not
    // inline — the dispatch loop advances by 1 after this returns.
    void execute_foreach_event(const ProgramSlot* slot,
                               std::span<const Instruction> program,
                               std::size_t ip);

    // Execute a BLOCK_CALL opcode (PRD prd-runtime-functions-control-flow L2).
    // Dispatches a shared user-`fn` body from slot->blocks[]: copies the
    // caller's argument buffers into the body's fixed convention param bank,
    // runs the body once under per-call-site state-ID isolation, then copies
    // the body output back into the caller's result buffer. The body lives in
    // the subprogram table, not inline — the dispatch loop advances by 1.
    // `binds` is the contiguous run of preceding BLOCK_BIND instructions
    // (empty for fns with <=5 params); each carries one logical param slot >=5.
    void execute_block_call(const ProgramSlot* slot,
                            std::span<const Instruction> program,
                            std::size_t ip,
                            std::span<const Instruction> binds = {});

    // Per-block cycle-position timing, computed once per block from the
    // upstream cycle length. Shared by run_voice_pool (voice gate-on/off
    // scheduling) and run_foreach_per_iteration (per-event onset/offset
    // sample computation for synthesized gate/trig).
    struct BlockCycleTiming {
        float spb = 0.0f;            // samples per beat
        float cycle_pos = 0.0f;      // cycle position at block start (beats)
        float block_end_pos = 0.0f;  // cycle position at block end (beats)
        std::uint32_t current_cycle = 0;
    };
    BlockCycleTiming compute_block_timing(float cycle_length) const;

    // Fill a 7-buffer event-record bank (PRD L3 event-record lambda params)
    // for one FOREACH_EVENT iteration. Layout, contiguous from bank_buf:
    //   +0 vel  +1 dur  +2 note  +3 chance  +4 time  +5 gate  +6 trig
    // gate/trig are synthesized per-sample from the event's cycle-relative
    // time/duration via `timing`; the rest are block constants.
    void fill_event_record_bank(std::uint16_t bank_buf,
                                const OutputEvents::OutputEvent& evt,
                                const BlockCycleTiming& timing);

    // Shared voice-pool engine: event scan, voice alloc/release, per-voice
    // body execution with XOR isolation, release-countdown mix. Used by both
    // execute_poly_block (inline body) and execute_foreach_event (table body).
    // bank_base is the first of 11 contiguous per-voice field buffers (indexed
    // by PatternPayload field id); voice_out_buf is the body's stereo L sink.
    void run_voice_pool(PolyAllocState& poly_state,
                        std::uint16_t mix_buf,
                        std::uint16_t bank_base,
                        std::uint16_t voice_out_buf,
                        std::span<const Instruction> body);

    // PER_ITERATION allocator: one body run per upstream event, per-iteration
    // XOR isolation, outputs mixed. SHARED allocator: fold accumulator threaded
    // through the body once per event.
    void run_foreach_per_iteration(ForeachIterState& iter_state,
                                   const Instruction& inst,
                                   std::span<const Instruction> body);
    void run_foreach_shared(ForeachSharedState& shared_state,
                            const Instruction& inst,
                            std::span<const Instruction> body);

    // Closure EVENT_MAP / EVENT_FILTER (PRD prd-runtime-event-transforms
    // Phase 2). For each upstream event the closure subprogram body runs once
    // under per-event XOR isolation: EVENT_MAP overlays the closure's returned
    // field bank onto a copied-through event; EVENT_FILTER keeps the event iff
    // the closure's predicate buffer is non-zero. The body lives in the
    // subprogram table — the dispatch loop advances by 1 after these return.
    void run_event_map_closure(const ProgramSlot* slot, const Instruction& inst);
    void run_event_filter_closure(const ProgramSlot* slot, const Instruction& inst);

    // Handle block-boundary swap logic
    void handle_swap();

    // Perform crossfade mixing
    void perform_crossfade(float* out_left, float* out_right);

    // Detect if structural change requires crossfade
    bool requires_crossfade(const ProgramSlot* old_slot,
                           const ProgramSlot* new_slot) const;

    // Rebind state IDs from old program to new program
    void rebind_states(const ProgramSlot* old_slot,
                      const ProgramSlot* new_slot);

    // Seek helpers
    void reconstruct_deterministic_states(std::uint64_t target_sample);
    void reset_history_dependent_states();
    void execute_preroll(std::uint32_t blocks);

    // Triple-buffer swap controller
    SwapController swap_controller_;

    // PRD L3: FOREACH_EVENT subprogram table staged for the next load.
    // Consumed and cleared by load_program / load_program_immediate.
    std::array<BlockEntry, MAX_SUBPROGRAMS> pending_blocks_{};
    std::uint32_t pending_block_count_ = 0;
    std::uint32_t pending_main_count_ = 0;
    bool has_pending_blocks_ = false;

    // Crossfade state
    CrossfadeState crossfade_state_;
    CrossfadeBuffers crossfade_buffers_;
    CrossfadeConfig crossfade_config_;

    // Execution context
    ExecutionContext ctx_;

    // Memory pools (owned)
    BufferPool buffer_pool_;
    StatePool state_pool_;
    // Shadow pool used by perform_crossfade(): snapshot of state_pool_
    // taken right before dual-execution so the OLD program's stateful
    // opcodes don't share-mutate state with the NEW program's. Without
    // this, OscState/EnvState/etc. get advanced twice per crossfade
    // block, producing a phase discontinuity at the swap boundary.
    StatePool shadow_state_pool_;
    EnvMap env_map_;
    SampleBank sample_bank_;
#ifndef CEDAR_NO_SOUNDFONT
    SoundFontRegistry soundfont_registry_;
#endif
#ifndef CEDAR_NO_FFT
    WavetableBankRegistry wavetable_registry_;
    // Per-block snapshot of all registered banks. Pinning shared_ptrs here
    // keeps banks alive through process_block() even if the host thread
    // swaps the registry. Refreshed each block in process_block().
    std::array<std::shared_ptr<const WavetableBank>,
                MAX_WAVETABLE_BANKS> wavetable_pins_{};
#endif
    AudioArena audio_arena_;
    // Shadow arena used together with shadow_state_pool_ for the crossfade
    // path. perform_crossfade() deep-copies both the active StatePool and
    // the audio arena's used bytes into these shadows, runs the old
    // program (which mutates state_pool_ + audio_arena_ contents), then
    // restores both. Without arena snapshotting, dual-execution would
    // corrupt delay/reverb buffer contents at positions both programs
    // wrote to in the same block. Same address space, only contents move
    // — pointers stored in DSP state structs remain valid.
    AudioArena shadow_audio_arena_;

    // Parsed MIDI files keyed by the name the akkado source uses (see
    // RequiredMidiSource::name_or_path). Pointer values live in audio_arena_;
    // the map is cleared whenever the arena is reset to avoid dangling
    // pointers. Touched only from the host thread (load_midi_file) and the
    // host-thread side of init_midi_queue_state — never from the audio path.
    std::unordered_map<std::string, MidiSequence*> midi_sequences_;
};

}  // namespace cedar
