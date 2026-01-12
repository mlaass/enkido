#include "../../include/cedar/opcodes/minblep.hpp"
#include <cmath>
#include <vector>

namespace cedar {

// Generate a simple MinBLEP table
// Single row repeated for all phases - no phase interpolation
// The residual smooths transitions without trying to handle sub-sample timing
std::array<float, MINBLEP_TABLE_SIZE> generate_minblep_table() {
    std::array<float, MINBLEP_TABLE_SIZE> table{};

    constexpr float pi = 3.14159265358979323846f;
    constexpr int oversampling = 32;
    constexpr float cutoff = 0.9f;
    constexpr int zero_crossings = 8;

    // Generate a single MinBLEP residual
    const int sinc_len = zero_crossings * 2 * oversampling;  // 512 samples
    const int center = zero_crossings * oversampling;         // 256

    std::vector<float> sinc(sinc_len, 0.0f);
    std::vector<float> bl_step(sinc_len, 0.0f);

    // Generate windowed sinc
    for (int i = 0; i < sinc_len; ++i) {
        float t = static_cast<float>(i - center) / static_cast<float>(oversampling);

        float sinc_val = (std::abs(t) < 1e-7f) ? cutoff
                       : (std::sin(pi * cutoff * t) / (pi * t));

        // Hann window
        float n = static_cast<float>(i) / static_cast<float>(sinc_len - 1);
        float window = 0.5f * (1.0f - std::cos(2.0f * pi * n));

        sinc[i] = sinc_val * window;
    }

    // Integrate to get step
    float sum = 0.0f;
    for (int i = 0; i < sinc_len; ++i) {
        sum += sinc[i];
        bl_step[i] = sum;
    }

    // Normalize
    if (std::abs(sum) > 1e-6f) {
        for (auto& v : bl_step) v /= sum;
    }

    // Create residuals: BL_step - 1.0 (post-step naive value)
    // Sample from center onwards (no pre-ringing)
    for (std::size_t p = 0; p < MINBLEP_PHASES; ++p) {
        float frac_pos = static_cast<float>(p) / static_cast<float>(MINBLEP_PHASES);

        for (std::size_t i = 0; i < MINBLEP_SAMPLES; ++i) {
            float sample_pos = static_cast<float>(i) - frac_pos;
            if (sample_pos < 0.0f) sample_pos = 0.0f;  // Clamp to step position

            int os_pos = center + static_cast<int>(std::round(sample_pos * oversampling));

            if (os_pos < sinc_len) {
                float bl = bl_step[os_pos];
                table[p * MINBLEP_SAMPLES + i] = bl - 1.0f;
            } else {
                table[p * MINBLEP_SAMPLES + i] = 0.0f;
            }
        }
    }

    return table;
}

const std::array<float, MINBLEP_TABLE_SIZE>& get_minblep_table() {
    static const auto table = generate_minblep_table();
    return table;
}

} // namespace cedar
