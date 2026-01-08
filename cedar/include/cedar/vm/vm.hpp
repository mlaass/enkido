#pragma once

#include "instruction.hpp"
#include "context.hpp"
#include "buffer_pool.hpp"
#include "state_pool.hpp"
#include <vector>
#include <span>

namespace cedar {

// Register-based bytecode VM for audio processing
// Processes entire blocks (128 samples) at a time for cache efficiency
class VM {
public:
    VM();
    ~VM();

    // Non-copyable (owns buffer pool and state pool)
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;
    VM(VM&&) noexcept = default;
    VM& operator=(VM&&) noexcept = default;

    // Load bytecode program (copies internally)
    bool load_program(std::span<const Instruction> bytecode);

    // Process one block of audio (128 samples)
    // Output buffers must have space for BLOCK_SIZE floats
    void process_block(float* output_left, float* output_right);

    // State management
    void reset();                    // Full reset (clear all state)
    void hot_swap_begin();           // Begin hot-swap (preserve matching states)
    void hot_swap_end();             // End hot-swap (GC orphaned states)

    // Configuration
    void set_sample_rate(float rate);
    void set_bpm(float bpm);

    // Accessors (for testing/debugging)
    [[nodiscard]] const ExecutionContext& context() const { return ctx_; }
    [[nodiscard]] BufferPool& buffers() { return buffer_pool_; }
    [[nodiscard]] StatePool& states() { return state_pool_; }

private:
    // Execute single instruction
    void execute(const Instruction& inst);

    // Program storage
    std::vector<Instruction> program_;

    // Execution context
    ExecutionContext ctx_;

    // Memory pools (owned)
    BufferPool buffer_pool_;
    StatePool state_pool_;
};

}  // namespace cedar
