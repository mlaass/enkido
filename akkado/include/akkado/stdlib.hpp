#pragma once

#include <cstddef>
#include <string_view>

namespace akkado {

/// Standard library source code, prepended to all user programs.
/// User definitions can shadow these.
constexpr std::string_view STDLIB_SOURCE = R"akkado(
// Akkado Standard Library
// User definitions can shadow these.

fn osc(type, freq, pwm = 0.5, phase = 0.0, trig = 0.0) -> match(type) {
    "sin": sine_osc(freq, phase, trig)
    "sine": sine_osc(freq, phase, trig)
    "tri": tri(freq, phase, trig)
    "triangle": tri(freq, phase, trig)
    "saw": saw(freq, phase, trig)
    "sawtooth": saw(freq, phase, trig)
    "sqr": sqr(freq, phase, trig)
    "square": sqr(freq, phase, trig)
    "ramp": ramp(freq, phase, trig)
    "phasor": phasor(freq, phase, trig)
    "noise": noise(freq, trig)
    "white": noise(freq, trig)
    "sqr_pwm": sqr_pwm(freq, pwm, phase, trig)
    "pulse": sqr_pwm(freq, pwm, phase, trig)
    "saw_pwm": saw_pwm(freq, pwm, phase, trig)
    "var_saw": saw_pwm(freq, pwm, phase, trig)
    "sqr_minblep": sqr_minblep(freq, phase, trig)
    "sqr_pwm_minblep": sqr_pwm_minblep(freq, pwm, phase, trig)
    _: sine_osc(freq, phase, trig)
}
)akkado";

/// Line count for diagnostic offset calculation (computed at compile time)
constexpr std::size_t STDLIB_LINE_COUNT = []() {
    std::size_t count = 1;
    for (char c : STDLIB_SOURCE) {
        if (c == '\n') ++count;
    }
    return count;
}();

/// Filename used for diagnostics originating from stdlib
constexpr std::string_view STDLIB_FILENAME = "<stdlib>";

} // namespace akkado
