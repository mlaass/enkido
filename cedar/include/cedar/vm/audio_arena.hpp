#pragma once

#include "../dsp/constants.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>

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
    static constexpr std::size_t DEFAULT_SIZE = 32 * 1024 * 1024;

    // Alignment for SIMD operations
    static constexpr std::size_t ALIGNMENT = 32;

    explicit AudioArena(std::size_t size = DEFAULT_SIZE)
        : size_(size)
        , offset_(0)
    {
        // size must be multiple of alignment for aligned_alloc
        std::size_t aligned_size = (size_ + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        memory_ = static_cast<float*>(std::aligned_alloc(ALIGNMENT, aligned_size));

        if (!memory_) {
            size_ = 0;  // Mark as invalid
            return;
        }
        std::memset(memory_, 0, aligned_size);
    }

    ~AudioArena() {
        if (memory_) {
            std::free(memory_);
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
                std::free(memory_);
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
