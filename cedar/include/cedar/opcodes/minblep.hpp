#pragma once

#include <array>
#include <cstddef>

namespace cedar {

// MinBLEP (Minimum-phase Band-Limited Step) table parameters
constexpr std::size_t MINBLEP_PHASES = 64;      // Oversampling factor
constexpr std::size_t MINBLEP_SAMPLES = 64;     // Length of the impulse response
constexpr std::size_t MINBLEP_TABLE_SIZE = MINBLEP_PHASES * MINBLEP_SAMPLES;

// Get the pre-computed MinBLEP table (generated at runtime, cached)
const std::array<float, MINBLEP_TABLE_SIZE>& get_minblep_table();

} // namespace cedar
