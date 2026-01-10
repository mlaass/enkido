#pragma once

#include "instruction.hpp"
#include "context.hpp"
#include "buffer_pool.hpp"
#include "state_pool.hpp"
#include "env_map.hpp"
#include "swap_controller.hpp"
#include "crossfade_state.hpp"
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
    // Query API
    // =========================================================================

    [[nodiscard]] bool is_crossfading() const;
    [[nodiscard]] float crossfade_position() const;
    [[nodiscard]] bool has_program() const;
    [[nodiscard]] std::uint32_t swap_count() const;

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
    AudioArena audio_arena_;
};

}  // namespace cedar
