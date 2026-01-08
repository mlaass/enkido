#pragma once

#include "../dsp/constants.hpp"
#include <array>
#include <algorithm>
#include <cstdint>

namespace cedar {

// Pre-allocated pool of audio buffers acting as "registers" for the VM
// All buffers are 32-byte aligned for SIMD operations
struct BufferPool {
    alignas(32) std::array<std::array<float, BLOCK_SIZE>, MAX_BUFFERS> buffers{};

    // Get pointer to buffer by index
    [[nodiscard]] float* get(std::uint16_t index) noexcept {
        return buffers[index].data();
    }

    [[nodiscard]] const float* get(std::uint16_t index) const noexcept {
        return buffers[index].data();
    }

    // Clear a specific buffer to zero
    void clear(std::uint16_t index) noexcept {
        std::fill_n(buffers[index].data(), BLOCK_SIZE, 0.0f);
    }

    // Clear all buffers
    void clear_all() noexcept {
        for (auto& buf : buffers) {
            std::fill(buf.begin(), buf.end(), 0.0f);
        }
    }

    // Fill buffer with constant value
    void fill(std::uint16_t index, float value) noexcept {
        std::fill_n(buffers[index].data(), BLOCK_SIZE, value);
    }

    // Copy one buffer to another
    void copy(std::uint16_t dst, std::uint16_t src) noexcept {
        std::copy_n(buffers[src].data(), BLOCK_SIZE, buffers[dst].data());
    }
};

}  // namespace cedar
