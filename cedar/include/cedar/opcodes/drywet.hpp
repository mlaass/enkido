#pragma once

#include <cstddef>

namespace cedar::drywet {

// Resolve a per-sample mix coefficient with sentinel fallback.
// Pass the buffer pointer (nullptr when inst.inputs[N] == 0xFFFF) and the
// fallback constant from the BuiltinInfo defaults entry.
[[gnu::always_inline]]
inline float coeff(const float* control_buf, std::size_t i, float fallback) {
    return control_buf ? control_buf[i] : fallback;
}

// Standard mix line used at the end of every effect inner loop.
// Category-A effects pass dry=1.0, wet=0.5 defaults via coeff();
// Category-B effects pass dry=0.0, wet=1.0.
[[gnu::always_inline]]
inline float mix(float dry_in, float processed, float dry, float wet) {
    return dry_in * dry + processed * wet;
}

}  // namespace cedar::drywet
