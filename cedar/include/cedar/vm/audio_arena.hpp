#pragma once

#include "../dsp/constants.hpp"
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
// - Simple bump allocator with 32-byte alignment for SIMD
// - Reset-based deallocation (no individual free)
// - Owned by StatePool and passed to states that need buffer memory
//
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
    {
        other.memory_ = nullptr;
        other.size_ = 0;
        other.offset_ = 0;
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
            other.memory_ = nullptr;
            other.size_ = 0;
            other.offset_ = 0;
        }
        return *this;
    }

    // Allocate a buffer of N floats from the arena
    // Returns nullptr if allocation fails (arena exhausted)
    [[nodiscard]] float* allocate(std::size_t num_floats) noexcept {
        if (!memory_) return nullptr;

        std::size_t bytes_needed = num_floats * sizeof(float);

        // Align offset to 32 bytes
        std::size_t aligned_offset = (offset_ + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

        if (aligned_offset + bytes_needed > size_) {
            // Arena exhausted
            return nullptr;
        }

        float* ptr = reinterpret_cast<float*>(reinterpret_cast<char*>(memory_) + aligned_offset);
        offset_ = aligned_offset + bytes_needed;

        // Zero the allocated memory
        std::memset(ptr, 0, bytes_needed);

        return ptr;
    }

    // Reset arena (invalidates all allocations)
    // Call when resetting the entire state pool
    void reset() noexcept {
        offset_ = 0;
        // Optionally zero memory for clean state
        if (memory_) {
            std::memset(memory_, 0, size_);
        }
    }

    // Query methods
    [[nodiscard]] std::size_t capacity() const noexcept { return size_; }
    [[nodiscard]] std::size_t used() const noexcept { return offset_; }
    [[nodiscard]] std::size_t available() const noexcept { return size_ - offset_; }
    [[nodiscard]] bool is_valid() const noexcept { return memory_ != nullptr; }

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
    float* memory_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
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
