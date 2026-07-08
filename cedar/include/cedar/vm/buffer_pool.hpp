#pragma once

#include "../dsp/constants.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace cedar {

struct Instruction;

namespace debug {
// Breadcrumb set by VM::execute()/execute_step() so the BufferPool guard
// below can name the instruction that passed a bad index. One thread_local
// store per instruction dispatch — negligible next to the 128-sample
// block loop each opcode runs.
inline thread_local const Instruction* current_inst = nullptr;
// Prints opcode + inputs of `current_inst` (defined in vm.cpp where the
// Instruction layout is visible).
void log_current_instruction();
}  // namespace debug

// Pre-allocated pool of audio buffers acting as "registers" for the VM.
//
// The pool is chunked into slabs (each SLAB_BUFFERS x BLOCK_SIZE floats,
// 32-byte aligned for SIMD). Slabs are allocated lazily so the pool can
// grow on hot-swap without invalidating buffer pointers the audio thread
// is reading: existing `std::unique_ptr<Slab>` entries never move, and
// fresh slabs are filled in by the compile thread before the new program
// is atomically published (so the audio thread only reads slab indices
// the compile thread has already armed).
//
// Slab 0 is preallocated by the VM constructor so BUFFER_ZERO (slot 255)
// is always valid.
struct BufferPool {
    struct alignas(32) Slab {
        std::array<std::array<float, BLOCK_SIZE>, SLAB_BUFFERS> data{};
    };

    BufferPool() {
        // Preallocate slab 0 so indices < SLAB_BUFFERS (including
        // BUFFER_ZERO = 255) are always live from VM construction
        // onwards. Subsequent slabs are grown on demand via
        // ensure_capacity() from the compile thread.
        ensure_capacity(SLAB_BUFFERS);
    }

    // Slabs are unique_ptr so existing entries are stable across growth.
    // `slabs_[i]` reads see a consistent pointer because the compile
    // thread writes happen-before the program-publish that the audio
    // thread acquires.
    std::array<std::unique_ptr<Slab>, MAX_SLABS> slabs_{};

    // Number of slabs currently allocated. Atomic so growth from the
    // compile thread synchronises-with any subsequent audio-thread read
    // that goes through the same atomic. The audio thread doesn't
    // *check* this — it trusts the published program's indices — but
    // diagnostics and tests do.
    std::atomic<std::uint32_t> active_slabs_{0};

    // Ensure the pool can hold at least `required` buffer indices. Idempotent.
    // Allocates fresh slabs (zero-initialised — `std::array<float, ...>{}`
    // value-init) on the calling thread. **Must not be called from the
    // audio thread** — heap allocation is forbidden there.
    void ensure_capacity(std::size_t required) {
        if (required == 0) return;
        if (required > MAX_BUFFERS) {
            std::printf("[CEDAR BUG] BufferPool::ensure_capacity(%zu) > MAX_BUFFERS=%zu\n",
                        required, MAX_BUFFERS);
            required = MAX_BUFFERS;
        }
        std::uint32_t needed = static_cast<std::uint32_t>(
            (required + SLAB_BUFFERS - 1) / SLAB_BUFFERS);
        std::uint32_t have = active_slabs_.load(std::memory_order_relaxed);
        for (std::uint32_t i = have; i < needed; ++i) {
            slabs_[i] = std::make_unique<Slab>();
        }
        if (needed > have) {
            active_slabs_.store(needed, std::memory_order_release);
        }
    }

    [[nodiscard]] std::uint32_t active_slabs() const noexcept {
        return active_slabs_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return MAX_BUFFERS;
    }

    // Get pointer to buffer by index. Bounds-checked; on out-of-range
    // (programmer bug or audio-thread read of an un-armed slab) logs
    // and returns the slab 0 buffer 0 pointer to avoid a hard crash.
    [[nodiscard]] float* get(std::uint16_t index) noexcept {
        if (index >= MAX_BUFFERS) {
            std::printf("[CEDAR BUG] BufferPool::get(%u) out of bounds (MAX_BUFFERS=%zu)\n",
                        static_cast<unsigned>(index), MAX_BUFFERS);
            debug::log_current_instruction();
            // Debug/sanitize builds fail loudly: an out-of-range index means
            // an opcode read an unwired input slot (0xFFFF) or the compiler
            // emitted one — both are bugs upstream, not runtime conditions.
            assert(false && "BufferPool::get out of bounds — unwired input reached the VM");
            return safe_fallback();
        }
        const std::uint32_t slab_idx = index / SLAB_BUFFERS;
        Slab* slab = slabs_[slab_idx].get();
        if (!slab) [[unlikely]] {
            std::printf("[CEDAR BUG] BufferPool::get(%u) reads un-armed slab %u\n",
                        static_cast<unsigned>(index), slab_idx);
            return safe_fallback();
        }
        return slab->data[index % SLAB_BUFFERS].data();
    }

    [[nodiscard]] const float* get(std::uint16_t index) const noexcept {
        if (index >= MAX_BUFFERS) {
            std::printf("[CEDAR BUG] BufferPool::get(%u) const out of bounds (MAX_BUFFERS=%zu)\n",
                        static_cast<unsigned>(index), MAX_BUFFERS);
            debug::log_current_instruction();
            assert(false && "BufferPool::get out of bounds — unwired input reached the VM");
            return safe_fallback_const();
        }
        const std::uint32_t slab_idx = index / SLAB_BUFFERS;
        const Slab* slab = slabs_[slab_idx].get();
        if (!slab) [[unlikely]] {
            std::printf("[CEDAR BUG] BufferPool::get(%u) const reads un-armed slab %u\n",
                        static_cast<unsigned>(index), slab_idx);
            return safe_fallback_const();
        }
        return slab->data[index % SLAB_BUFFERS].data();
    }

    // Clear a specific buffer to zero. Triggers slab allocation if needed
    // (used by VM constructor to clear BUFFER_ZERO).
    void clear(std::uint16_t index) noexcept {
        ensure_capacity(static_cast<std::size_t>(index) + 1);
        std::fill_n(get(index), BLOCK_SIZE, 0.0f);
    }

    // Clear every live buffer to zero.
    void clear_all() noexcept {
        const std::uint32_t live = active_slabs_.load(std::memory_order_acquire);
        for (std::uint32_t s = 0; s < live; ++s) {
            if (Slab* slab = slabs_[s].get(); slab) {
                for (auto& buf : slab->data) {
                    std::fill(buf.begin(), buf.end(), 0.0f);
                }
            }
        }
    }

    // Fill buffer with constant value.
    void fill(std::uint16_t index, float value) noexcept {
        ensure_capacity(static_cast<std::size_t>(index) + 1);
        std::fill_n(get(index), BLOCK_SIZE, value);
    }

    // Copy one buffer to another.
    void copy(std::uint16_t dst, std::uint16_t src) noexcept {
        std::copy_n(get(src), BLOCK_SIZE, get(dst));
    }

private:
    // Defensive fallback: every BufferPool keeps a single in-line scratch
    // buffer so `get()` of an out-of-range or un-armed index returns a
    // valid pointer (matches the legacy fixed-array behaviour, which
    // returned `buffers[0].data()` in the same situation).
    alignas(32) mutable std::array<float, BLOCK_SIZE> fallback_{};
    [[nodiscard]] float* safe_fallback() noexcept { return fallback_.data(); }
    [[nodiscard]] const float* safe_fallback_const() const noexcept { return fallback_.data(); }
};

}  // namespace cedar
