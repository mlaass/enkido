#pragma once

#include <cstddef>
#include <string_view>

namespace akkado {

/// Standard library source code, prepended to all user programs.
/// User definitions can shadow these.
constexpr std::string_view STDLIB_SOURCE = R"akkado(
// Akkado Standard Library
// User definitions can shadow these.

fn osc(type, freq, pwm = 0.5) -> match(type) {
    "sin": sine_osc(freq)
    "sine": sine_osc(freq)
    "tri": tri(freq)
    "triangle": tri(freq)
    "saw": saw(freq)
    "sawtooth": saw(freq)
    "sqr": sqr(freq)
    "square": sqr(freq)
    "ramp": ramp(freq)
    "phasor": phasor(freq)
    "noise": noise()
    "white": noise()
    "sqr_pwm": sqr_pwm(freq, pwm)
    "pulse": sqr_pwm(freq, pwm)
    "saw_pwm": saw_pwm(freq, pwm)
    "var_saw": saw_pwm(freq, pwm)
    "sqr_minblep": sqr_minblep(freq)
    "sqr_pwm_minblep": sqr_pwm_minblep(freq, pwm)
    _: sine_osc(freq)
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
