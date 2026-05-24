#pragma once

#include <cstddef>
#include <string_view>

#include "akkado/generated/stdlib_files.hpp"

namespace akkado {

/// Standard library source code, prepended to all user programs.
/// User definitions can shadow these.
constexpr std::string_view STDLIB_SOURCE = R"akkado(
// Akkado Standard Library
// User definitions can shadow these.

fn osc(type, freq, pwm = 0.5, phase = 0.0, trig = 0.0) -> match(type) {
    "sin": sine(freq, phase, trig)
    "sine": sine(freq, phase, trig)
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
    "sqr_pwm_4x": sqr_pwm_4x(freq, pwm, phase, trig)
    "saw_pwm_4x": saw_pwm_4x(freq, pwm, phase, trig)
    _: sine(freq, phase, trig)
}

fn multiband3fx(s, f1, f2, fx_lo, fx_mid, fx_hi) -> {
    fx_lo(lp(lp(s, f1), f1)) + fx_mid(lp(lp(hp(hp(s, f1), f1), f2), f2)) + fx_hi(hp(hp(s, f2), f2))
}

fn beat(n) -> {trigger(1/n)}

// unison(freq, gate, vel, instrument, voices, detune, width, phase)
// Multiplies a single note across N detuned, panned, phase-shifted voices.
// Returns a stereo signal. `instrument` is a 4-arg function (freq, gate, vel, ext)
// where ext = {idx, count, detune_st, pan, phase}.
//   voices  - compile-time integer literal, sane range 1..16 (soft limit:
//             larger values compile but grow the AST; voices<=0 collapses
//             linspace to a single centered voice).
//   detune  - per-voice frequency spread in SEMITONES (runtime signal/const).
//   width   - per-voice stereo pan spread, -width..+width (runtime).
//   phase   - per-voice initial-phase spread, 0..phase cycles (runtime).
//
// The voice sum is scaled by 1/sqrt(voices) so the overall RMS stays roughly
// constant as `voices` grows (supersaw convention — voices are partially
// correlated, so sqrt(N) is a workable compromise between linear sum-of-
// identical-signals and the sqrt(N) sum-of-uncorrelated-signals).
fn unison(freq, gate, vel, instrument,
          voices = 2, detune = 0.5, width = 0.5, phase = 0) -> match(voices) {
    // voices == 1: linspace(-1,1,1) collapses to [-1] rather than [0], so the
    // general path would produce an off-center detuned voice. Special-case it
    // to a single centered, undetuned, in-phase voice. sqrt(1) = 1, so no
    // gain compensation needed here.
    1: pan(instrument(freq, gate, vel,
                      { idx: 0, count: 1, detune_st: 0, pan: 0, phase: 0 }), 0)
    _: {
        unit = linspace(-1, 1, voices)
        map(unit, (u, idx) -> {
            d_st = u * detune
            v_freq = freq * pow(2, d_st / 12)
            v_pan = u * width
            v_phase = (idx / voices) * phase
            ext = {
                idx: idx,
                count: voices,
                detune_st: d_st,
                pan: v_pan,
                phase: v_phase
            }
            pan(instrument(v_freq, gate, vel, ext), v_pan)
        }) |> sum(%) * (1 / sqrt(voices))
    }
}

// glide(s, time, curve, space) — time-based interpolation with optional
// curve shape and value-space conversion.
//
//   curve: "linear" (default) | "ease_in" | "ease_out" | "cosine" / "cos"
//   space: "linear" (default) | "log"  — use "log" for pitch-uniform glide
//
// The curve+space match is inlined here (rather than delegated to a helper
// fn) because param_literals_ is cleared at each user-fn boundary; nested
// match keeps both `space` and `curve` resolvable at compile time. Default
// time = 0.05 (50 ms) gives `glide(@freq)` a subtle baseline portamento
// without forcing users to pick a number. 1.4426950408889634 = 1/ln(2)
// converts natural log (the `log` builtin) to log₂.
fn glide(s, time = 0.05, curve = "linear", space = "linear") -> match(space) {
    "log": match(curve) {
        "linear":   pow(2, interp(log(s) * 1.4426950408889634, time))
        "ease_in":  pow(2, interp_ease_in(log(s) * 1.4426950408889634, time))
        "ease_out": pow(2, interp_ease_out(log(s) * 1.4426950408889634, time))
        "cosine":   pow(2, interp_cos(log(s) * 1.4426950408889634, time))
        "cos":      pow(2, interp_cos(log(s) * 1.4426950408889634, time))
        _:          pow(2, interp(log(s) * 1.4426950408889634, time))
    }
    _: match(curve) {
        "linear":   interp(s, time)
        "ease_in":  interp_ease_in(s, time)
        "ease_out": interp_ease_out(s, time)
        "cosine":   interp_cos(s, time)
        "cos":      interp_cos(s, time)
        _:          interp(s, time)
    }
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
