#pragma once

#include <cstddef>
#include <cstdint>

namespace cedar {

// Audio processing constants
inline constexpr float PI = 3.14159265358979323846f;
inline constexpr float TWO_PI = 6.28318530717958647692f;
inline constexpr float HALF_PI = 1.57079632679489661923f;

// Block processing
inline constexpr std::size_t BLOCK_SIZE = 128;
inline constexpr float DEFAULT_SAMPLE_RATE = 48000.0f;
inline constexpr float DEFAULT_BPM = 120.0f;

// Memory limits
inline constexpr std::size_t MAX_BUFFERS = 256;
inline constexpr std::size_t MAX_STATES = 256;
inline constexpr std::size_t MAX_VARS = 4096;
inline constexpr std::size_t MAX_PROGRAM_SIZE = 4096;
inline constexpr std::size_t MAX_ENV_PARAMS = 256;

// Special buffer indices
inline constexpr std::uint16_t BUFFER_UNUSED = 0xFFFF;

// Reserved buffer index for constant zero (always contains 0.0)
// Used as default for optional inputs like phase offset and trigger
// This buffer is reserved and should NEVER be used for program data
inline constexpr std::uint16_t BUFFER_ZERO = 255;  // Last buffer in pool

// Rate flags
inline constexpr std::uint8_t RATE_AUDIO = 0;    // Process every sample
inline constexpr std::uint8_t RATE_CONTROL = 1;  // Process once per block

}  // namespace cedar
