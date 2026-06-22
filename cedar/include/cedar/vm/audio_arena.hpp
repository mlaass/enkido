#pragma once

#include "../dsp/constants.hpp"
#include "../util/log_once.hpp"
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace cedar {

// Pre-allocated memory arena for audio buffers (delay lines, reverb buffers, etc.)
// Guarantees zero heap allocation during audio processing.
//
// Design:
// - Bump allocator with 32-byte alignment for SIMD, plus a size-classed,
//   allocation-free free list so evicted DSP states can return their buffers
//   and a later same-class allocation reuses them (prd-audio-arena-reclamation).
//   Without this the bump pointer only grows; a long live-coding session of
//   structurally distinct hot-swaps eventually exhausts the arena.
// - reset() rewinds the bump pointer AND clears the free lists.
// - Owned by StatePool and passed to states that need buffer memory.
//
// Free list: blocks are rounded up to a power-of-two float count (size class);
// allocate() bumps the rounded size so every class-C block is interchangeable,
// and release() pushes the block onto its class via an intrusive next-pointer
// stored in the block's own memory (zero heap). Reused blocks are re-zeroed.
class AudioArena {
public:
    // Default size: 32MB (enough for ~8 large reverbs + many delays)
    // Overridable via CEDAR_ARENA_SIZE compile definition
#ifdef CEDAR_ARENA_SIZE
    static constexpr std::size_t DEFAULT_SIZE = CEDAR_ARENA_SIZE;
#else
    static constexpr std::size_t DEFAULT_SIZE = 32 * 1024 * 1024;
#endif

    // Alignment for SIMD operations
    static constexpr std::size_t ALIGNMENT = 32;

    explicit AudioArena(std::size_t size = DEFAULT_SIZE)
        : size_(size)
        , offset_(0)
    {
        // size must be multiple of alignment for aligned_alloc
        std::size_t aligned_size = (size_ + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
#if defined(_MSC_VER)
        memory_ = static_cast<float*>(_aligned_malloc(aligned_size, ALIGNMENT));
#elif defined(CEDAR_USE_POSIX_MEMALIGN)
        void* raw = nullptr;
        if (posix_memalign(&raw, ALIGNMENT, aligned_size) != 0) {
            raw = nullptr;
        }
        memory_ = static_cast<float*>(raw);
#else
        memory_ = static_cast<float*>(std::aligned_alloc(ALIGNMENT, aligned_size));
#endif

        if (!memory_) {
            size_ = 0;  // Mark as invalid
            return;
        }
        std::memset(memory_, 0, aligned_size);
    }

    ~AudioArena() {
        if (memory_) {
#if defined(_MSC_VER)
            _aligned_free(memory_);
#else
            std::free(memory_);
#endif
        }
    }

    // Non-copyable, movable
    AudioArena(const AudioArena&) = delete;
    AudioArena& operator=(const AudioArena&) = delete;

    AudioArena(AudioArena&& other) noexcept
        : memory_(other.memory_)
        , size_(other.size_)
        , offset_(other.offset_)
        , exhaustion_count_(other.exhaustion_count_)
    {
        // Free-list heads point into memory_, which moves with us unchanged.
        std::memcpy(free_lists_, other.free_lists_, sizeof(free_lists_));
        std::memcpy(free_count_, other.free_count_, sizeof(free_count_));
        other.memory_ = nullptr;
        other.size_ = 0;
        other.offset_ = 0;
        std::memset(other.free_lists_, 0, sizeof(other.free_lists_));
        std::memset(other.free_count_, 0, sizeof(other.free_count_));
    }

    AudioArena& operator=(AudioArena&& other) noexcept {
        if (this != &other) {
            if (memory_) {
#if defined(_MSC_VER)
                _aligned_free(memory_);
#else
                std::free(memory_);
#endif
            }
            memory_ = other.memory_;
            size_ = other.size_;
            offset_ = other.offset_;
            exhaustion_count_ = other.exhaustion_count_;
            std::memcpy(free_lists_, other.free_lists_, sizeof(free_lists_));
            std::memcpy(free_count_, other.free_count_, sizeof(free_count_));
            other.memory_ = nullptr;
            other.size_ = 0;
            other.offset_ = 0;
            std::memset(other.free_lists_, 0, sizeof(other.free_lists_));
            std::memset(other.free_count_, 0, sizeof(other.free_count_));
        }
        return *this;
    }

    // Allocate a buffer of N floats from the arena.
    // Returns nullptr if allocation fails (arena exhausted).
    //
    // Pooled sizes (n <= MAX_CLASS floats) are rounded up to a power-of-two
    // size class and satisfied from the free list when a same-class block is
    // available (re-zeroed on reuse); otherwise bump-allocated at the rounded
    // size so freed class-C blocks are interchangeable. Sizes larger than any
    // class fall back to an exact, un-pooled bump allocation (legacy behavior).
    [[nodiscard]] float* allocate(std::size_t num_floats) noexcept {
        if (!memory_) return nullptr;

        const int cls = size_class(num_floats);
        if (cls <= MAX_CLASS) {
            const std::size_t floats = std::size_t{1} << cls;
            if (free_lists_[cls]) {
                FreeNode* node = free_lists_[cls];
                free_lists_[cls] = node->next;
                --free_count_[cls];
                std::memset(node, 0, floats * sizeof(float));  // re-zero on reuse
                return reinterpret_cast<float*>(node);
            }
            return bump(floats);
        }
        return bump(num_floats);  // too big to pool: exact bump
    }

    // Return a previously-allocated block to its size-class free list so a
    // later same-class allocate() reuses it. `num_floats` MUST be the value
    // passed to allocate(). No-op on null. Free-list overflow (more than
    // FREE_CAP_PER_CLASS blocks held for a class) drops the block — it stays in
    // bump space until the next reset() — and logs once. Never frees to the OS.
    void release(float* ptr, std::size_t num_floats) noexcept {
        if (!ptr) return;
        const int cls = size_class(num_floats);
        if (cls > MAX_CLASS) return;  // un-pooled allocation: nothing to reclaim
        if (free_count_[cls] >= FREE_CAP_PER_CLASS) {
            CEDAR_LOG_ONCE("[CEDAR] audio arena free-list overflow (class %d): block dropped until reset\n", cls);
            return;
        }
        FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_lists_[cls];
        free_lists_[cls] = node;
        ++free_count_[cls];
    }

    // Reset arena (invalidates all allocations and clears the free lists).
    // Call when resetting the entire state pool.
    void reset() noexcept {
        offset_ = 0;
        exhaustion_count_ = 0;
        for (int c = 0; c <= MAX_CLASS; ++c) { free_lists_[c] = nullptr; free_count_[c] = 0; }
        // Optionally zero memory for clean state
        if (memory_) {
            std::memset(memory_, 0, size_);
        }
    }

    // Query methods
    [[nodiscard]] std::size_t capacity() const noexcept { return size_; }
    [[nodiscard]] std::size_t used() const noexcept { return offset_; }
    // Bump high-water in bytes. With reclamation this plateaus once the free
    // list satisfies reallocations — the bounded-memory signal (§3.6).
    [[nodiscard]] std::size_t bytes_used() const noexcept { return offset_; }
    [[nodiscard]] std::size_t available() const noexcept { return size_ - offset_; }
    [[nodiscard]] bool is_valid() const noexcept { return memory_ != nullptr; }
    // Count of allocate() calls that failed (no free block + no bump room).
    // Stays 0 while reclamation keeps the working set within the arena; a
    // non-zero value is the exhaustion the drift fuzz guards against.
    [[nodiscard]] std::size_t exhaustion_count() const noexcept { return exhaustion_count_; }

    // Check if a pointer belongs to this arena
    [[nodiscard]] bool owns(const float* ptr) const noexcept {
        if (!memory_ || !ptr) return false;
        const char* p = reinterpret_cast<const char*>(ptr);
        const char* base = reinterpret_cast<const char*>(memory_);
        return p >= base && p < base + size_;
    }

    // Deep-copy the used portion of `src` into this arena, including its
    // bump-allocator offset. Used by the VM's crossfade path to snapshot
    // every DSP buffer (delay lines, reverb tanks, sample voices, etc.)
    // owned by stateful opcodes so the dual-execution of old + new
    // programs against shared arena memory doesn't corrupt the state new
    // expects to see at the swap moment.
    //
    // Pointers stored in DSP state structs reference src.memory_+offset
    // positions. Those pointers remain valid after a memcpy back into the
    // SAME arena (this method writes into THIS arena, not into src's),
    // because the base address is unchanged — only contents move.
    //
    // Requires this arena to be at least as large as src.used(). Returns
    // false if not. Caller is responsible for matching the snapshot/
    // restore pair on the same two arenas to keep the offset in sync.
    [[nodiscard]] bool copy_used_from(const AudioArena& src) noexcept {
        if (!memory_ || !src.memory_) return false;
        if (src.offset_ > size_) return false;
        std::memcpy(memory_, src.memory_, src.offset_);
        offset_ = src.offset_;
        return true;
    }

private:
    // Raw bump allocation of `floats` floats (32-byte aligned, zeroed).
    [[nodiscard]] float* bump(std::size_t floats) noexcept {
        const std::size_t bytes_needed = floats * sizeof(float);
        const std::size_t aligned_offset = (offset_ + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        if (aligned_offset + bytes_needed > size_) {
            ++exhaustion_count_;  // no free block AND no bump room — true exhaustion
            return nullptr;
        }
        float* ptr = reinterpret_cast<float*>(reinterpret_cast<char*>(memory_) + aligned_offset);
        offset_ = aligned_offset + bytes_needed;
        std::memset(ptr, 0, bytes_needed);
        return ptr;
    }

    // Smallest size-class shift `s` with (1<<s) >= n, floored at MIN_CLASS.
    // Returns a value > MAX_CLASS for sizes too large to pool (caller bumps
    // exactly). The longest real allocation (a 960k-sample delay line) lands at
    // class 20; anything larger is a test-only edge that stays un-pooled.
    [[nodiscard]] static int size_class(std::size_t n) noexcept {
        int s = MIN_CLASS;
        while ((std::size_t{1} << s) < n) ++s;
        return s;
    }

    // Intrusive free-list node stored in the freed block's own first bytes.
    struct FreeNode { FreeNode* next; };

    static constexpr int MIN_CLASS = 3;   // 8 floats (32 bytes) — holds a pointer
    static constexpr int MAX_CLASS = 21;  // 2,097,152 floats — covers MAX_DELAY_SAMPLES (960k)
    static constexpr std::size_t FREE_CAP_PER_CLASS = 256;

    float* memory_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
    std::size_t exhaustion_count_ = 0;
    FreeNode* free_lists_[MAX_CLASS + 1] = {};
    std::uint16_t free_count_[MAX_CLASS + 1] = {};
};

// Buffer handle for states - just a pointer and size
// The pointer points into the arena and is NOT owned
struct ArenaBuffer {
    float* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool is_valid() const noexcept { return data != nullptr && size > 0; }

    void clear() noexcept {
        if (data && size > 0) {
            std::memset(data, 0, size * sizeof(float));
        }
    }

    float& operator[](std::size_t i) noexcept { return data[i]; }
    const float& operator[](std::size_t i) const noexcept { return data[i]; }
};

}  // namespace cedar
