#pragma once

#include <cstddef>
#include <cstdint>

namespace cedar {

// Audio processing constants
inline constexpr float PI = 3.14159265358979323846f;
inline constexpr float TWO_PI = 6.28318530717958647692f;
inline constexpr float HALF_PI = 1.57079632679489661923f;

// Block processing (overridable via CEDAR_BLOCK_SIZE)
#ifdef CEDAR_BLOCK_SIZE
inline constexpr std::size_t BLOCK_SIZE = CEDAR_BLOCK_SIZE;
#else
inline constexpr std::size_t BLOCK_SIZE = 128;
#endif
inline constexpr float DEFAULT_SAMPLE_RATE = 48000.0f;
inline constexpr float DEFAULT_BPM = 120.0f;

// Memory limits (overridable via CEDAR_MAX_* defines)
#ifdef CEDAR_MAX_BUFFERS
inline constexpr std::size_t MAX_BUFFERS = CEDAR_MAX_BUFFERS;
#else
inline constexpr std::size_t MAX_BUFFERS = 256;
#endif

#ifdef CEDAR_MAX_STATES
inline constexpr std::size_t MAX_STATES = CEDAR_MAX_STATES;
#else
inline constexpr std::size_t MAX_STATES = 512;
#endif

#ifdef CEDAR_MAX_VARS
inline constexpr std::size_t MAX_VARS = CEDAR_MAX_VARS;
#else
inline constexpr std::size_t MAX_VARS = 4096;
#endif

#ifdef CEDAR_MAX_PROGRAM_SIZE
inline constexpr std::size_t MAX_PROGRAM_SIZE = CEDAR_MAX_PROGRAM_SIZE;
#else
inline constexpr std::size_t MAX_PROGRAM_SIZE = 4096;
#endif

#ifdef CEDAR_MAX_ENV_PARAMS
inline constexpr std::size_t MAX_ENV_PARAMS = CEDAR_MAX_ENV_PARAMS;
#else
inline constexpr std::size_t MAX_ENV_PARAMS = 256;
#endif

// Subprogram table (PRD prd-runtime-functions-control-flow L3). FOREACH_EVENT
// block bodies live in the subprogram table on each ProgramSlot. MAX_SUBPROGRAMS
// bounds the number of distinct FOREACH/POLY blocks in one program; body
// instructions share the MAX_PROGRAM_SIZE budget (no separate arena).
#ifdef CEDAR_MAX_SUBPROGRAMS
inline constexpr std::size_t MAX_SUBPROGRAMS = CEDAR_MAX_SUBPROGRAMS;
#else
inline constexpr std::size_t MAX_SUBPROGRAMS = 64;
#endif

// Per-block body instruction-count cap (matches the legacy POLY body cap).
#ifdef CEDAR_MAX_BLOCK_BODY
inline constexpr std::size_t MAX_BLOCK_BODY = CEDAR_MAX_BLOCK_BODY;
#else
inline constexpr std::size_t MAX_BLOCK_BODY = 255;
#endif

// Special buffer indices
inline constexpr std::uint16_t BUFFER_UNUSED = 0xFFFF;

// Reserved buffer index for constant zero (always contains 0.0)
// Used as default for optional inputs like phase offset and trigger
// This buffer is reserved and should NEVER be used for program data
inline constexpr std::uint16_t BUFFER_ZERO = static_cast<std::uint16_t>(MAX_BUFFERS - 1);

// Rate flags
inline constexpr std::uint8_t RATE_AUDIO = 0;    // Process every sample
inline constexpr std::uint8_t RATE_CONTROL = 1;  // Process once per block

}  // namespace cedar
