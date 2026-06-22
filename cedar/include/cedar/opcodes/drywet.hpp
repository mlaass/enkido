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

// Dry passthrough for the arena-exhaustion fallback: copy the input straight
// to the stereo output. Mono input (stereo_in == false) is broadcast to both
// lanes. Used by every arena-backed FX opcode when its buffers failed to
// allocate, so the effect degrades to a pass-through instead of dereferencing
// a null buffer. See prd-audio-arena-reclamation §3.5.
[[gnu::always_inline]]
inline void passthrough_stereo(const float* in_l, const float* in_r,
                               bool stereo_in, float* out_l, float* out_r,
                               std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const float x = in_l[i];
        out_l[i] = x;
        out_r[i] = stereo_in ? in_r[i] : x;
    }
}

}  // namespace cedar::drywet
