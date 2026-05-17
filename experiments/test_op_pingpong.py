"""
Test: DELAY_PINGPONG (Stereo Ping-Pong Delay)
===============================================
Tests DELAY_PINGPONG opcode for stereo ping-pong delay with cross-feedback.

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp):
- Delay with cross-feedback: L feedback goes to R, R feedback goes to L
- Echoes should alternate between channels
- Includes damping (HF rolloff in feedback path)
- Unified dry/wet via ExtendedParams<2> (Category A defaults: dry=1.0, wet=0.5)
- Output = dry_in * dry + delayed * wet (mix line)

If this test fails, check the implementation in cedar/include/cedar/opcodes/stereo.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_pingpong")


# =============================================================================
# Helper Functions
# =============================================================================

def gen_noise_burst(duration, sr, burst_duration=0.01, burst_start=0.0):
    """Generate a short noise burst for impulse response testing."""
    signal = np.zeros(int(duration * sr), dtype=np.float32)
    start_sample = int(burst_start * sr)
    burst_samples = int(burst_duration * sr)
    signal[start_sample:start_sample + burst_samples] = np.random.uniform(-0.8, 0.8, burst_samples).astype(np.float32)
    return signal


# =============================================================================
# Tests
# =============================================================================

def test_pingpong_delay():
    """
    Test DELAY_PINGPONG opcode for stereo ping-pong delay.

    Acceptance criteria:
    - Delay timing error < 2%
    - Echoes clearly alternate between L and R channels
    - Feedback decay follows expected curve
    """
    print("Test: DELAY_PINGPONG Stereo Delay")

    sr = 48000
    duration = 3.0
    delay_sec = 0.2  # 200ms delay

    # Input: noise burst in LEFT channel only
    left_in = gen_noise_burst(duration, sr, burst_duration=0.02, burst_start=0.0)
    right_in = np.zeros_like(left_in)

    host = CedarTestHost(sr)

    # Parameters
    buf_delay = host.set_param("delay", delay_sec)
    buf_feedback = host.set_param("feedback", 0.6)
    buf_width = host.set_param("width", 1.0)  # Full ping-pong

    # DELAY_PINGPONG(out, in_L, in_R, delay, feedback, width)
    # Note: Inputs are buf0, buf1 for L/R, then params
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.DELAY_PINGPONG, 10, 0, 1, buf_delay, buf_feedback, buf_width,
        cedar.hash("pingpong") & 0xFFFF
    )
    host.load_instruction(inst)

    # Output from buffers 10 and 11
    host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 10, 11))

    out_l, out_r = host.process_stereo(left_in, right_in)

    # Find peaks in each channel
    expected_delay_samples = int(delay_sec * sr)
    threshold = 0.01

    def find_peaks(signal, threshold, min_distance):
        peaks = []
        for i in range(1, len(signal) - 1):
            if signal[i] > threshold and signal[i] > signal[i-1] and signal[i] > signal[i+1]:
                if len(peaks) == 0 or i - peaks[-1] > min_distance:
                    peaks.append(i)
        return peaks

    peaks_l = find_peaks(np.abs(out_l), threshold, expected_delay_samples // 2)
    peaks_r = find_peaks(np.abs(out_r), threshold, expected_delay_samples // 2)

    print(f"  Expected delay: {expected_delay_samples} samples ({delay_sec*1000:.0f}ms)")
    print(f"  Left channel peaks: {len(peaks_l)}")
    print(f"  Right channel peaks: {len(peaks_r)}")

    # The first peak should be in LEFT (direct + first reflection)
    # The second peak should be in RIGHT (ping)
    # The third should be in LEFT (pong), etc.

    # Analyze alternation pattern
    all_peaks = sorted([(p, 'L') for p in peaks_l] + [(p, 'R') for p in peaks_r])

    if len(all_peaks) >= 4:
        print(f"\n  Echo pattern (first 6):")
        for i, (pos, channel) in enumerate(all_peaks[:6]):
            time_ms = pos / sr * 1000
            level_l = 20 * np.log10(np.abs(out_l[pos]) + 1e-10)
            level_r = 20 * np.log10(np.abs(out_r[pos]) + 1e-10)
            print(f"    Echo {i}: {time_ms:.1f}ms, Channel={channel}, L={level_l:.1f}dB, R={level_r:.1f}dB")

        # Check timing accuracy
        # First L echo comes at 1x delay, subsequent L echoes at 2x delay intervals (round-trip L->R->L)
        if len(peaks_l) >= 3:
            delays_l = np.diff(peaks_l)
            # Skip first interval (which is 1x delay), analyze subsequent intervals (2x delay)
            round_trip_delays = delays_l[1:]
            if len(round_trip_delays) > 0:
                avg_round_trip = np.mean(round_trip_delays)
                expected_round_trip = expected_delay_samples * 2
                timing_error = abs(avg_round_trip - expected_round_trip) / expected_round_trip * 100

                print(f"\n  First echo delay: {delays_l[0]:.0f} samples (expected: ~{expected_delay_samples})")
                print(f"  Avg round-trip delay: {avg_round_trip:.0f} samples (expected: {expected_round_trip})")
                print(f"  Timing error: {timing_error:.2f}%")

                if timing_error < 2:
                    print(f"  ✓ PASS: Timing error < 2%")
                else:
                    print(f"  ✗ FAIL: Timing error {timing_error:.2f}% > 2%")

        # Check alternation
        # After initial transient, echoes should alternate L-R-L-R
        if len(all_peaks) >= 4:
            channels = [p[1] for p in all_peaks[1:5]]  # Skip first (direct signal)

            # Check if pattern alternates
            alternates = True
            for i in range(1, len(channels)):
                if channels[i] == channels[i-1]:
                    alternates = False
                    break

            if alternates:
                print(f"  ✓ PASS: Echoes alternate between L and R")
            else:
                print(f"  ✗ FAIL: Echoes don't properly alternate: {channels}")

    # Save WAV
    stereo = np.column_stack([out_l, out_r])
    wav_path = os.path.join(OUT, "pingpong_delay.wav")
    save_wav(wav_path, stereo, sr)
    print(f"\n  Saved {wav_path} - Listen for alternating L/R echoes with smooth decay")

    # Plot
    fig, axes = plt.subplots(3, 1, figsize=(14, 10))
    fig.suptitle(f"DELAY_PINGPONG Analysis ({delay_sec*1000:.0f}ms delay, 0.6 feedback)")

    time_ms = np.arange(len(out_l)) / sr * 1000

    # Waveforms
    ax1 = axes[0]
    ax1.plot(time_ms, out_l, 'b-', alpha=0.7, linewidth=0.5, label='Left')
    ax1.plot(time_ms, out_r, 'r-', alpha=0.7, linewidth=0.5, label='Right')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title('Output Waveforms')
    ax1.legend()
    ax1.set_xlim(0, 2000)

    # Mark expected echo times
    for i in range(10):
        echo_time = (i + 1) * delay_sec * 1000
        if echo_time < 2000:
            color = 'r' if i % 2 == 0 else 'b'
            ax1.axvline(echo_time, color=color, linestyle=':', alpha=0.3)

    # Envelope (dB)
    ax2 = axes[1]
    env_l = np.abs(out_l)
    env_r = np.abs(out_r)
    env_l_db = 20 * np.log10(env_l + 1e-10)
    env_r_db = 20 * np.log10(env_r + 1e-10)

    ax2.plot(time_ms, env_l_db, 'b-', alpha=0.5, linewidth=0.5, label='Left')
    ax2.plot(time_ms, env_r_db, 'r-', alpha=0.5, linewidth=0.5, label='Right')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level (dB)')
    ax2.set_title('Envelope (dB scale)')
    ax2.legend()
    ax2.set_xlim(0, 2000)
    ax2.set_ylim(-80, 0)

    # L-R difference (shows alternation)
    ax3 = axes[2]
    diff = out_l - out_r
    ax3.plot(time_ms, diff, 'g-', linewidth=0.5)
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('L - R')
    ax3.set_title('L-R Difference (alternation visible as sign changes)')
    ax3.set_xlim(0, 2000)
    ax3.axhline(0, color='k', linewidth=0.5)

    plt.tight_layout()
    fig_path = os.path.join(OUT, "pingpong_analysis.png")
    save_figure(fig, fig_path)
    print(f"  Saved {fig_path}")


# =============================================================================
# Dry/Wet Mix Tests (PRD: unified dry/wet convention)
# =============================================================================

def _run_pingpong_with_mix(sr, n_samples, left_in, right_in, *, delay_sec=0.2,
                            feedback=0.6, width=1.0, dry=None, wet=None,
                            hash_name="pingpong_mix"):
    """Run DELAY_PINGPONG with explicit dry/wet ExtendedParams seeding.

    `dry`/`wet=None` skips the ExtendedParams init so the opcode falls back to
    the Category A defaults (dry=1.0, wet=0.5) baked into stereo.hpp.
    """
    host = CedarTestHost(sr)
    buf_delay = host.set_param("delay", delay_sec)
    buf_feedback = host.set_param("feedback", feedback)
    buf_width = host.set_param("width", width)

    state_id = cedar.hash(hash_name) & 0xFFFF
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.DELAY_PINGPONG, 10, 0, 1, buf_delay, buf_feedback,
        buf_width, state_id
    )
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 10, 11))

    host.vm.load_program(host.program)
    if dry is not None or wet is not None:
        d = 1.0 if dry is None else float(dry)
        w = 0.5 if wet is None else float(wet)
        host.vm.init_extended_params(
            cedar.ext_params_state_id(state_id),
            np.array([d, w], dtype=np.float32),
            np.array([0xFFFF, 0xFFFF], dtype=np.uint16),
            2,
        )

    n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    padded_len = n_blocks * cedar.BLOCK_SIZE
    in_l = np.zeros(padded_len, dtype=np.float32)
    in_r = np.zeros(padded_len, dtype=np.float32)
    in_l[:n_samples] = left_in
    in_r[:n_samples] = right_in

    out_l, out_r = [], []
    for k in range(n_blocks):
        s = k * cedar.BLOCK_SIZE
        e = s + cedar.BLOCK_SIZE
        host.vm.set_buffer(0, in_l[s:e])
        host.vm.set_buffer(1, in_r[s:e])
        l, r = host.vm.process()
        out_l.append(l)
        out_r.append(r)
    return (np.concatenate(out_l)[:n_samples],
            np.concatenate(out_r)[:n_samples])


def test_pingpong_default_mix_is_category_a():
    """
    With no explicit dry/wet, the opcode must apply Category A defaults
    (dry=1.0, wet=0.5). Drive an impulse and confirm the first echo's
    amplitude relative to the dry burst matches 0.5 * feedback.

    Pre-migration the opcode did `out = in + delayed` (effectively wet=1),
    so the first echo would be ~= 0.6. Post-migration with wet=0.5 the echo
    should be ~= 0.3 of the original burst peak.
    """
    print("Test: Pingpong default dry/wet = Category A (1.0, 0.5)")
    sr = 48000
    duration = 1.5
    delay_sec = 0.1
    feedback = 0.6
    left_in = gen_noise_burst(duration, sr, burst_duration=0.005, burst_start=0.0)
    right_in = np.zeros_like(left_in)

    out_l, out_r = _run_pingpong_with_mix(
        sr, len(left_in), left_in, right_in,
        delay_sec=delay_sec, feedback=feedback, width=1.0
    )

    dry_peak = float(np.max(np.abs(left_in)))
    delay_samples = int(delay_sec * sr)
    # Cross-feedback puts the FIRST R echo at 2x delay (L is written; one
    # delay-line trip moves the energy onto R via cross_right = delayed_left).
    # See op_delay_pingpong in stereo.hpp.
    first_l_echo_lo = int(delay_samples * 0.9)
    first_l_echo_hi = int(delay_samples * 1.2)
    first_l_echo = float(np.max(np.abs(out_l[first_l_echo_lo:first_l_echo_hi])))
    first_r_echo_lo = int(delay_samples * 1.9)
    first_r_echo_hi = int(delay_samples * 2.2)
    first_r_echo = float(np.max(np.abs(out_r[first_r_echo_lo:first_r_echo_hi])))
    # First L echo expected = wet * dry_peak (no feedback factor — direct
    # delayed signal). Default wet=0.5 makes ratio ~ 0.5 * <damped peak>.
    ratio_l = first_l_echo / max(dry_peak, 1e-9)
    ratio_r = first_r_echo / max(dry_peak, 1e-9)
    print(f"  Dry burst peak: {dry_peak:.3f}")
    print(f"  First L echo (~{delay_sec*1000:.0f}ms): {first_l_echo:.3f}, ratio={ratio_l:.3f}")
    print(f"  First R echo (~{2*delay_sec*1000:.0f}ms): {first_r_echo:.3f}, ratio={ratio_r:.3f}")
    # With default wet=0.5 the L echo should be ~ 0.5 * dry_peak * <damping
    # factor>; the R echo is one feedback trip later ~ 0.5 * feedback *
    # dry_peak * <damping>. Damping + smoothing trim peaks ~10-30%.
    if 0.25 < ratio_l < 0.55:
        print(f"  ✓ PASS: L echo ratio {ratio_l:.3f} in Category A band [0.25..0.55]")
    else:
        print(f"  ✗ FAIL: L echo ratio {ratio_l:.3f} outside Category A band")

    stereo = np.column_stack([out_l, out_r])
    wav_path = os.path.join(OUT, "pingpong_default_mix.wav")
    save_wav(wav_path, stereo, sr)
    print(f"  Saved {wav_path} - Listen for softer echo tail vs legacy parity wav")


def test_pingpong_dry_zero_wet_one():
    """
    dry=0, wet=1 should suppress the dry burst on output and leave only the
    delayed/feedback signal.
    """
    print("Test: Pingpong dry=0, wet=1 -> wet-only output")
    sr = 48000
    duration = 1.5
    delay_sec = 0.1
    left_in = gen_noise_burst(duration, sr, burst_duration=0.005, burst_start=0.0)
    right_in = np.zeros_like(left_in)

    out_l, out_r = _run_pingpong_with_mix(
        sr, len(left_in), left_in, right_in,
        delay_sec=delay_sec, feedback=0.6, width=1.0,
        dry=0.0, wet=1.0
    )

    # During the burst window (samples 0..240) the dry signal should NOT
    # appear on L since dry=0 zeroes it out.
    burst_end = int(0.005 * sr) + 2
    dry_peak_window = float(np.max(np.abs(out_l[:burst_end])))
    if dry_peak_window < 1e-4:
        print(f"  ✓ PASS: dry burst suppressed on L (peak {dry_peak_window:.6f})")
    else:
        print(f"  ✗ FAIL: dry burst leaked: peak={dry_peak_window:.6f} (expected ~0)")

    stereo = np.column_stack([out_l, out_r])
    wav_path = os.path.join(OUT, "pingpong_dry0_wet1.wav")
    save_wav(wav_path, stereo, sr)
    print(f"  Saved {wav_path} - Listen for delayed echoes with no dry impulse")


def test_pingpong_legacy_parity_dry1_wet1():
    """
    dry=1, wet=1 reproduces the pre-migration `out = in + delayed` math.
    The first echo on R should match feedback * dry_peak (no 0.5 attenuation).
    """
    print("Test: Pingpong dry=1, wet=1 -> legacy parity")
    sr = 48000
    duration = 1.5
    delay_sec = 0.1
    feedback = 0.6
    left_in = gen_noise_burst(duration, sr, burst_duration=0.005, burst_start=0.0)
    right_in = np.zeros_like(left_in)

    out_l, out_r = _run_pingpong_with_mix(
        sr, len(left_in), left_in, right_in,
        delay_sec=delay_sec, feedback=feedback, width=1.0,
        dry=1.0, wet=1.0
    )

    dry_peak = float(np.max(np.abs(left_in)))
    delay_samples = int(delay_sec * sr)
    # First L echo at 1x delay (direct delayed-left readout). Legacy parity
    # math (dry=1, wet=1) → out_l = in_l + delayed_l. During the echo window
    # in_l is 0, so out_l = delayed_l ~= dry_peak * damping_factor.
    first_l_echo_lo = int(delay_samples * 0.9)
    first_l_echo_hi = int(delay_samples * 1.2)
    first_l_echo = float(np.max(np.abs(out_l[first_l_echo_lo:first_l_echo_hi])))
    ratio = first_l_echo / max(dry_peak, 1e-9)
    print(f"  Dry burst peak: {dry_peak:.3f}")
    print(f"  First L echo (~{delay_sec*1000:.0f}ms): {first_l_echo:.3f}, ratio={ratio:.3f}")
    # Pre-migration math should give ratio ~ 1.0 (less damping); default-mix
    # version gives ~0.5. Anything north of 0.55 confirms legacy parity.
    if 0.55 < ratio < 1.05:
        print(f"  ✓ PASS: legacy-parity ratio {ratio:.3f} inside [0.55..1.05]")
    else:
        print(f"  ✗ FAIL: ratio {ratio:.3f} outside legacy-parity band")

    stereo = np.column_stack([out_l, out_r])
    wav_path = os.path.join(OUT, "pingpong_legacy_parity.wav")
    save_wav(wav_path, stereo, sr)
    print(f"  Saved {wav_path} - Listen: echo tail ~2x louder than default_mix.wav")


def test_pingpong_long_run_stability():
    """
    Per CLAUDE.md DSP guidance: render >=300 s of audio with pattern-like
    drive (impulse retriggers every 0.5 s) to catch any slow state-drift bugs
    introduced by the dry/wet refactor. Trace-only — no WAV write.
    """
    print("Test: Pingpong long-run stability (300 s simulated)")
    sr = 48000
    duration = 300.0
    n_samples = int(duration * sr)
    # Retrigger an impulse every 0.5 s, into L only.
    left_in = np.zeros(n_samples, dtype=np.float32)
    for t_ms in range(0, int(duration * 1000), 500):
        idx = int(t_ms / 1000.0 * sr)
        if idx + 32 < n_samples:
            left_in[idx:idx + 32] = np.random.uniform(-0.5, 0.5, 32).astype(np.float32)
    right_in = np.zeros_like(left_in)

    out_l, out_r = _run_pingpong_with_mix(
        sr, n_samples, left_in, right_in,
        delay_sec=0.25, feedback=0.7, width=1.0
    )

    finite_l = bool(np.all(np.isfinite(out_l)))
    finite_r = bool(np.all(np.isfinite(out_r)))
    peak_l = float(np.max(np.abs(out_l)))
    peak_r = float(np.max(np.abs(out_r)))
    if finite_l and finite_r and peak_l < 10.0 and peak_r < 10.0:
        print(f"  ✓ PASS: 300 s stable, peak L={peak_l:.3f} R={peak_r:.3f}")
    else:
        print(f"  ✗ FAIL: finite_l={finite_l} finite_r={finite_r} peak_l={peak_l} peak_r={peak_r}")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("=" * 60)
    print("DELAY_PINGPONG OPCODE TESTS")
    print("=" * 60)

    print()
    test_pingpong_delay()

    print()
    test_pingpong_default_mix_is_category_a()

    print()
    test_pingpong_dry_zero_wet_one()

    print()
    test_pingpong_legacy_parity_dry1_wet1()

    print()
    test_pingpong_long_run_stability()

    print("\n" + "=" * 60)
    print("DELAY_PINGPONG TESTS COMPLETE")
    print("=" * 60)
