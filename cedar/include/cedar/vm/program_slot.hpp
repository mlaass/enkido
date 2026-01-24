#pragma once

#include "instruction.hpp"
#include "../dsp/constants.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace cedar {

// Packed metadata about a program for structural change detection
struct ProgramSignature {
    std::uint32_t dag_hash = 0;           // FNV-1a of all semantic IDs in order
    std::uint32_t instruction_count = 0;
    std::uint32_t state_id_count = 0;

    bool operator==(const ProgramSignature& other) const noexcept {
        return dag_hash == other.dag_hash &&
               instruction_count == other.instruction_count &&
               state_id_count == other.state_id_count;
    }

    bool operator!=(const ProgramSignature& other) const noexcept {
        return !(*this == other);
    }
};

// A single program buffer slot for triple buffering
struct alignas(64) ProgramSlot {  // Cache-line aligned
    // Slot ownership state
    enum class State : std::uint8_t {
        Empty,      // No program loaded
        Loading,    // Compiler writing (owned by compiler thread)
        Ready,      // Available for swap
        Active,     // Currently being executed by audio thread
        Fading      // Being crossfaded out (previous slot)
    };

    // Program bytecode storage (fixed-size, no allocations)
    std::array<Instruction, MAX_PROGRAM_SIZE> instructions{};
    std::uint32_t instruction_count = 0;

    // Program signature for change detection
    ProgramSignature signature{};

    // Set of semantic IDs in this program (for GC and change detection)
    // Using fixed array instead of set to avoid allocations
    std::array<std::uint16_t, MAX_STATES> state_ids{};
    std::uint32_t state_id_count = 0;

    // Slot state (atomic for thread-safe ownership transfer)
    std::atomic<State> state{State::Empty};

    // Generation counter for ABA prevention
    std::atomic<std::uint32_t> generation{0};

    // Clear the slot
    void clear() noexcept {
        instruction_count = 0;
        state_id_count = 0;
        signature = {};
        generation.fetch_add(1, std::memory_order_relaxed);
        state.store(State::Empty, std::memory_order_release);
    }

    // Load program from span (called by compiler thread)
    // Returns false if program too large
    bool load(std::span<const Instruction> bytecode) noexcept {
        if (bytecode.size() > MAX_PROGRAM_SIZE) {
            return false;
        }

        instruction_count = static_cast<std::uint32_t>(bytecode.size());
        std::copy(bytecode.begin(), bytecode.end(), instructions.begin());

        // Extract unique state IDs and compute signature
        compute_signature();

        return true;
    }

    // Compute signature from loaded program
    void compute_signature() noexcept {
        state_id_count = 0;
        std::uint32_t dag_hash = 2166136261u;  // FNV-1a offset basis

        for (std::uint32_t i = 0; i < instruction_count; ++i) {
            const auto& inst = instructions[i];

            // Only track instructions that have state
            if (inst.state_id != 0) {
                // Add to hash
                dag_hash ^= inst.state_id;
                dag_hash *= 16777619u;  // FNV-1a prime

                // Add to unique state IDs (check for duplicates)
                bool found = false;
                for (std::uint32_t j = 0; j < state_id_count; ++j) {
                    if (state_ids[j] == inst.state_id) {
                        found = true;
                        break;
                    }
                }
                if (!found && state_id_count < MAX_STATES) {
                    state_ids[state_id_count++] = inst.state_id;
                }
            }
        }

        signature.dag_hash = dag_hash;
        signature.instruction_count = instruction_count;
        signature.state_id_count = state_id_count;
    }

    // Check if state ID exists in this program
    [[nodiscard]] bool has_state_id(std::uint16_t id) const noexcept {
        for (std::uint32_t i = 0; i < state_id_count; ++i) {
            if (state_ids[i] == id) {
                return true;
            }
        }
        return false;
    }

    // Get span of instructions for execution
    [[nodiscard]] std::span<const Instruction> program() const noexcept {
        return {instructions.data(), instruction_count};
    }

    // Get span of state IDs
    [[nodiscard]] std::span<const std::uint16_t> get_state_ids() const noexcept {
        return {state_ids.data(), state_id_count};
    }
};

}  // namespace cedar
