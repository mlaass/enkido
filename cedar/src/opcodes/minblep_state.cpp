#include "../../include/cedar/opcodes/dsp_state.hpp"
#include "../../include/cedar/opcodes/minblep.hpp"
#include <algorithm>

namespace cedar {

void MinBLEPOscState::add_step(float amplitude, float frac_pos, const float* minblep_table,
                                std::size_t table_phases, std::size_t samples_per_phase) {
    // Determine which phase to use based on fractional position
    std::size_t phase = static_cast<std::size_t>(frac_pos * static_cast<float>(table_phases));
    phase = std::min(phase, table_phases - 1);
    
    // Add the residual to the buffer
    for (std::size_t i = 0; i < samples_per_phase; ++i) {
        std::size_t buf_idx = (write_pos + i) % BUFFER_SIZE;
        buffer[buf_idx] += amplitude * minblep_table[phase * samples_per_phase + i];
    }
}

} // namespace cedar
