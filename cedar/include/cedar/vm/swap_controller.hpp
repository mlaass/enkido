#pragma once

#include "program_slot.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace cedar {

// Triple-buffer swap controller with lock-free operations
// Enables glitch-free program updates during audio playback
class SwapController {
public:
    SwapController() {
        // Initialize slot 0 as current (active but empty)
        slots_[0].state.store(ProgramSlot::State::Active, std::memory_order_relaxed);
        slots_[1].state.store(ProgramSlot::State::Empty, std::memory_order_relaxed);
        slots_[2].state.store(ProgramSlot::State::Empty, std::memory_order_relaxed);
    }

    ~SwapController() = default;

    // Non-copyable
    SwapController(const SwapController&) = delete;
    SwapController& operator=(const SwapController&) = delete;

    // =========================================================================
    // Compiler Thread API (called from compiler/main thread)
    // =========================================================================

    // Acquire a slot for writing new program
    // Returns nullptr if no slot available (should not happen with triple buffer)
    [[nodiscard]] ProgramSlot* acquire_write_slot() noexcept {
        // Find an empty slot
        for (auto& slot : slots_) {
            auto expected = ProgramSlot::State::Empty;
            if (slot.state.compare_exchange_strong(expected, ProgramSlot::State::Loading,
                    std::memory_order_acq_rel)) {
                return &slot;
            }
        }
        return nullptr;  // All slots busy (should not happen)
    }

    // Submit written slot as ready for swap
    // Returns true if successful
    bool submit_ready(ProgramSlot* slot) noexcept {
        if (!slot) return false;

        auto expected = ProgramSlot::State::Loading;
        if (slot->state.compare_exchange_strong(expected, ProgramSlot::State::Ready,
                std::memory_order_acq_rel)) {
            swap_pending_.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    // Load program in one step (acquire + load + submit)
    // Convenience method for compiler thread
    bool load_program(std::span<const Instruction> bytecode) noexcept {
        ProgramSlot* slot = acquire_write_slot();
        if (!slot) return false;

        if (!slot->load(bytecode)) {
            slot->state.store(ProgramSlot::State::Empty, std::memory_order_release);
            return false;
        }

        return submit_ready(slot);
    }

    // =========================================================================
    // Audio Thread API (called from audio thread only)
    // =========================================================================

    // Check if a new program is ready for swap
    [[nodiscard]] bool has_pending_swap() const noexcept {
        return swap_pending_.load(std::memory_order_acquire);
    }

    // Execute swap at block boundary
    // Returns true if swap occurred
    bool execute_swap() noexcept {
        if (!swap_pending_.load(std::memory_order_acquire)) {
            return false;
        }

        // Find the ready slot
        ProgramSlot* ready_slot = nullptr;
        for (auto& slot : slots_) {
            if (slot.state.load(std::memory_order_acquire) == ProgramSlot::State::Ready) {
                ready_slot = &slot;
                break;
            }
        }

        if (!ready_slot) {
            swap_pending_.store(false, std::memory_order_release);
            return false;
        }

        // Get current slot
        std::uint8_t curr_idx = current_idx_.load(std::memory_order_acquire);
        ProgramSlot* curr_slot = &slots_[curr_idx];

        // Current becomes previous (fading)
        curr_slot->state.store(ProgramSlot::State::Fading, std::memory_order_release);
        previous_idx_.store(curr_idx, std::memory_order_release);

        // Ready becomes current (active)
        ready_slot->state.store(ProgramSlot::State::Active, std::memory_order_release);

        // Find the index of the ready slot
        std::uint8_t ready_idx = static_cast<std::uint8_t>(ready_slot - slots_.data());
        current_idx_.store(ready_idx, std::memory_order_release);

        // Clear pending flag
        swap_pending_.store(false, std::memory_order_release);

        // Increment swap counter
        swap_count_.fetch_add(1, std::memory_order_relaxed);

        return true;
    }

    // Get current program slot for execution
    [[nodiscard]] ProgramSlot* current_slot() noexcept {
        return &slots_[current_idx_.load(std::memory_order_acquire)];
    }

    [[nodiscard]] const ProgramSlot* current_slot() const noexcept {
        return &slots_[current_idx_.load(std::memory_order_acquire)];
    }

    // Get previous slot (for crossfade source)
    [[nodiscard]] ProgramSlot* previous_slot() noexcept {
        std::uint8_t prev_idx = previous_idx_.load(std::memory_order_acquire);
        auto& slot = slots_[prev_idx];
        if (slot.state.load(std::memory_order_acquire) == ProgramSlot::State::Fading) {
            return &slot;
        }
        return nullptr;
    }

    [[nodiscard]] const ProgramSlot* previous_slot() const noexcept {
        std::uint8_t prev_idx = previous_idx_.load(std::memory_order_acquire);
        const auto& slot = slots_[prev_idx];
        if (slot.state.load(std::memory_order_acquire) == ProgramSlot::State::Fading) {
            return &slot;
        }
        return nullptr;
    }

    // Mark previous slot as no longer needed (after crossfade completes)
    void release_previous() noexcept {
        std::uint8_t prev_idx = previous_idx_.load(std::memory_order_acquire);
        auto& slot = slots_[prev_idx];
        if (slot.state.load(std::memory_order_acquire) == ProgramSlot::State::Fading) {
            slot.clear();
        }
    }

    // =========================================================================
    // Query API (thread-safe)
    // =========================================================================

    // Check if any program is loaded
    [[nodiscard]] bool has_program() const noexcept {
        const auto* slot = current_slot();
        return slot && slot->instruction_count > 0;
    }

    // Get swap count
    [[nodiscard]] std::uint32_t swap_count() const noexcept {
        return swap_count_.load(std::memory_order_relaxed);
    }

private:
    // The three slots
    std::array<ProgramSlot, 3> slots_;

    // Atomic indices
    std::atomic<std::uint8_t> current_idx_{0};
    std::atomic<std::uint8_t> previous_idx_{1};

    // Flag indicating a swap is ready
    std::atomic<bool> swap_pending_{false};

    // Statistics
    std::atomic<std::uint32_t> swap_count_{0};
};

}  // namespace cedar
