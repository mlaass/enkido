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
#include <memory>
#include <span>

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

    // Configure iter()/iterBack() rotation on an existing SequenceState.
    // Called after init_sequence_program_state when iter_n > 0.
    void init_sequence_iter_state(std::uint32_t state_id,
                                  std::uint8_t n, std::int8_t dir) {
        state_pool_.init_sequence_iter(state_id, n, dir);
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

    // Initialize polyphony state for a POLY_BEGIN opcode
    void init_poly_state(std::uint32_t state_id, std::uint32_t seq_state_id,
                         std::uint8_t max_voices, std::uint8_t mode,
                         std::uint8_t steal_strategy) {
        auto& state = state_pool_.get_or_create<PolyAllocState>(state_id);
        state.seq_state_id = seq_state_id;
        state.max_voices = std::min(max_voices, static_cast<std::uint8_t>(PolyAllocState::MAX_VOICES));
        state.mode = mode;
        state.steal_strategy = steal_strategy;
        state.ensure_voices(&audio_arena_);
    }

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
                               const char* /*name_or_path*/,
                               std::uint8_t channel_filter,
                               bool loop,
                               MidiQueueState::TempoMode tempo) {
        auto& s = state_pool_.get_or_create<MidiQueueState>(state_id);

        s.kind           = kind;
        s.channel_filter = channel_filter;
        s.loop           = loop;
        s.tempo_mode     = tempo;

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
    }

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

    // Execute a POLY block — iterates voices, sets XOR isolation, accumulates mix
    // Returns the instruction pointer past POLY_END
    std::size_t execute_poly_block(std::span<const Instruction> program, std::size_t ip);

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

    // Crossfade state
    CrossfadeState crossfade_state_;
    CrossfadeBuffers crossfade_buffers_;
    CrossfadeConfig crossfade_config_;

    // Execution context
    ExecutionContext ctx_;

    // Memory pools (owned)
    BufferPool buffer_pool_;
    StatePool state_pool_;
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
};

}  // namespace cedar
