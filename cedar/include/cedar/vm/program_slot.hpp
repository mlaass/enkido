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
    std::uint32_t content_hash = 0;       // FNV-1a of every instruction byte
    std::uint32_t instruction_count = 0;
    std::uint32_t state_id_count = 0;

    bool operator==(const ProgramSignature& other) const noexcept {
        return dag_hash == other.dag_hash &&
               content_hash == other.content_hash &&
               instruction_count == other.instruction_count &&
               state_id_count == other.state_id_count;
    }

    bool operator!=(const ProgramSignature& other) const noexcept {
        return !(*this == other);
    }
};

// Subprogram table entry (PRD prd-runtime-functions-control-flow L3).
// A FOREACH_EVENT block body lives inside the same `instructions` array, in
// the region after the main program; this entry locates it. `offset` is an
// absolute index into ProgramSlot::instructions.
struct BlockEntry {
    std::uint16_t offset = 0;            // index of body's first instruction
    std::uint16_t length = 0;            // body instruction count
    std::uint16_t frame_slot_count = 0;  // # convention input slots the body reads
    std::uint8_t  output_count = 1;      // 1=mono, 2=stereo
    std::uint8_t  reserved = 0;
};
static_assert(sizeof(BlockEntry) == 8, "BlockEntry must be 8 bytes");

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

    // Program bytecode storage (fixed-size, no allocations).
    // Layout: [ main program | block body 0 | block body 1 | ... ].
    // `instruction_count` covers main + all bodies; `main_count` is the
    // main/body boundary — the dispatch loop runs only [0, main_count).
    std::array<Instruction, MAX_PROGRAM_SIZE> instructions{};
    std::uint32_t instruction_count = 0;
    std::uint32_t main_count = 0;

    // Subprogram table — FOREACH_EVENT block bodies (PRD L3).
    std::array<BlockEntry, MAX_SUBPROGRAMS> blocks{};
    std::uint32_t block_count = 0;

    // Program signature for change detection
    ProgramSignature signature{};

    // Set of semantic IDs in this program (for GC and change detection)
    // Using fixed array instead of set to avoid allocations
    std::array<std::uint32_t, MAX_STATES> state_ids{};
    std::uint32_t state_id_count = 0;

    // Slot state (atomic for thread-safe ownership transfer)
    std::atomic<State> state{State::Empty};

    // Generation counter for ABA prevention
    std::atomic<std::uint32_t> generation{0};

    // Clear the slot
    void clear() noexcept {
        instruction_count = 0;
        main_count = 0;
        block_count = 0;
        state_id_count = 0;
        signature = {};
        generation.fetch_add(1, std::memory_order_relaxed);
        state.store(State::Empty, std::memory_order_release);
    }

    // Load program from span (called by compiler thread).
    // Returns false if program too large. No subprogram table — `main_count`
    // equals `instruction_count` and `block_count` is zero (backward
    // compatible: programs with no FOREACH_EVENT blocks use this path).
    bool load(std::span<const Instruction> bytecode) noexcept {
        if (bytecode.size() > MAX_PROGRAM_SIZE) {
            return false;
        }

        instruction_count = static_cast<std::uint32_t>(bytecode.size());
        main_count = instruction_count;
        block_count = 0;
        std::copy(bytecode.begin(), bytecode.end(), instructions.begin());

        // Extract unique state IDs and compute signature
        compute_signature();

        return true;
    }

    // Load program with a subprogram table (PRD L3). `bytecode` is the full
    // [ main | bodies ] instruction stream; `main_inst_count` is the
    // main/body boundary; `block_table` locates each FOREACH_EVENT body.
    bool load(std::span<const Instruction> bytecode,
              std::span<const BlockEntry> block_table,
              std::uint32_t main_inst_count) noexcept {
        if (bytecode.size() > MAX_PROGRAM_SIZE) {
            return false;
        }
        if (block_table.size() > MAX_SUBPROGRAMS) {
            return false;
        }
        if (main_inst_count > bytecode.size()) {
            return false;
        }

        instruction_count = static_cast<std::uint32_t>(bytecode.size());
        main_count = main_inst_count;
        block_count = static_cast<std::uint32_t>(block_table.size());
        std::copy(bytecode.begin(), bytecode.end(), instructions.begin());
        std::copy(block_table.begin(), block_table.end(), blocks.begin());

        compute_signature();

        return true;
    }

    // Compute signature from loaded program
    void compute_signature() noexcept {
        state_id_count = 0;
        std::uint32_t dag_hash = 2166136261u;  // FNV-1a offset basis
        std::uint32_t content_hash = 2166136261u;
        constexpr std::uint32_t FNV_PRIME = 16777619u;

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(instructions.data());
        const std::size_t total_bytes = static_cast<std::size_t>(instruction_count) * sizeof(Instruction);
        for (std::size_t i = 0; i < total_bytes; ++i) {
            content_hash ^= bytes[i];
            content_hash *= FNV_PRIME;
        }

        for (std::uint32_t i = 0; i < instruction_count; ++i) {
            const auto& inst = instructions[i];

            // Only track instructions that have state
            if (inst.state_id != 0) {
                // Add to hash
                dag_hash ^= inst.state_id;
                dag_hash *= FNV_PRIME;

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

        // Fold the subprogram table into content_hash — two programs with
        // identical instruction bytes but different block boundaries are
        // structurally different programs (PRD L3 §6).
        content_hash ^= block_count;
        content_hash *= FNV_PRIME;
        content_hash ^= main_count;
        content_hash *= FNV_PRIME;
        const auto* block_bytes = reinterpret_cast<const std::uint8_t*>(blocks.data());
        const std::size_t block_table_bytes =
            static_cast<std::size_t>(block_count) * sizeof(BlockEntry);
        for (std::size_t i = 0; i < block_table_bytes; ++i) {
            content_hash ^= block_bytes[i];
            content_hash *= FNV_PRIME;
        }

        signature.dag_hash = dag_hash;
        signature.content_hash = content_hash;
        signature.instruction_count = instruction_count;
        signature.state_id_count = state_id_count;
    }

    // Check if state ID exists in this program
    [[nodiscard]] bool has_state_id(std::uint32_t id) const noexcept {
        for (std::uint32_t i = 0; i < state_id_count; ++i) {
            if (state_ids[i] == id) {
                return true;
            }
        }
        return false;
    }

    // Get span of all instructions (main program + subprogram bodies).
    // FOREACH_EVENT body dispatch indexes into this full span; the main
    // dispatch loop bounds itself to `main_count`.
    [[nodiscard]] std::span<const Instruction> program() const noexcept {
        return {instructions.data(), instruction_count};
    }

    // Get the instruction span of FOREACH_EVENT block `id`, or an empty span
    // if `id` is out of range.
    [[nodiscard]] std::span<const Instruction> block_body(std::uint32_t id) const noexcept {
        if (id >= block_count) {
            return {};
        }
        const BlockEntry& b = blocks[id];
        if (static_cast<std::size_t>(b.offset) + b.length > instruction_count) {
            return {};
        }
        return {instructions.data() + b.offset, b.length};
    }

    // Get span of state IDs
    [[nodiscard]] std::span<const std::uint32_t> get_state_ids() const noexcept {
        return {state_ids.data(), state_id_count};
    }
};

}  // namespace cedar
