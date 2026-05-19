"""
Compressor Quality Tests (Cedar Engine)
========================================
Tests for DYNAMICS_COMP opcode.
Validates threshold accuracy, ratio, attack/release timing, and gain reduction.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, db_to_linear, linear_to_db, save_wav
from visualize import save_figure

OUT = output_dir("op_comp")


def gen_test_tone(freq, duration, sr, amplitude=1.0):
    """Generate a test sine tone."""
    t = np.arange(int(duration * sr)) / sr
    return (np.sin(2 * np.pi * freq * t) * amplitude).astype(np.float32)


def gen_level_sweep(duration, sr, start_db=-60, end_db=0, freq=1000):
    """Generate a tone with linearly increasing level in dB."""
    t = np.arange(int(duration * sr)) / sr
    # Linear ramp in dB space
    db_levels = np.linspace(start_db, end_db, len(t))
    amplitudes = db_to_linear(db_levels)
    return (np.sin(2 * np.pi * freq * t) * amplitudes).astype(np.float32)


def measure_rms_blocks(signal, sr, block_ms=50):
    """Measure RMS level in blocks."""
    block_samples = int(block_ms / 1000 * sr)
    num_blocks = len(signal) // block_samples
    rms_values = []
    for i in range(num_blocks):
        start = i * block_samples
        end = start + block_samples
        block = signal[start:end]
        rms = np.sqrt(np.mean(block ** 2))
        rms_values.append(rms)
    return np.array(rms_values)


def _build_comp(host, threshold_db, ratio, attack_ms=0.1, release_ms=10.0,
                init_ext=True, state_name="comp"):
    """Configure host with a DYNAMICS_COMP instruction.

    attack/release moved from inst.rate bit-packing to ExtendedParams<2>
    (prd-extended-params-migration §4.1). When init_ext is False the
    ExtendedParams state is left uninitialized to exercise the opcode's
    nullptr fallback (attack=0.1ms, release=10ms).
    buf_in=0, buf_out=1.
    """
    buf_thresh = host.set_param("threshold", threshold_db)
    buf_ratio = host.set_param("ratio", ratio)
    state_id = cedar.hash(state_name) & 0xFFFF
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.DYNAMICS_COMP, 1, 0, buf_thresh, buf_ratio, state_id
    )
    inst.rate = 0  # rate field no longer carries attack/release
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))
    host.vm.load_program(host.program)
    if init_ext:
        host.vm.init_extended_params(
            cedar.ext_params_state_id(state_id),
            np.array([attack_ms, release_ms], dtype=np.float32),
            np.array([0xFFFF, 0xFFFF], dtype=np.uint16),
            2,
        )
    return state_id


# =============================================================================
# DYNAMICS_COMP Test - Compressor Ratio and Threshold
# =============================================================================

def test_compressor_ratio():
    """
    Test compressor reduces gain above threshold by ratio.
    - Input: 1kHz tone at varying levels (-60dB to 0dB)
    - Settings: threshold=-20dB, ratio=4:1
    - Above threshold, +4dB in yields +1dB out
    """
    print("Test 1: DYNAMICS_COMP (Compressor) Ratio and Threshold")
    print("=" * 60)

    sr = 48000
    duration = 5.0
    freq = 1000

    # Compressor settings
    threshold_db = -20
    ratio = 4.0
    attack_ms = 10
    release_ms = 100

    results = {'sample_rate': sr, 'tests': [], 'transfer_curve': []}

    # Test transfer curve: measure output for various input levels
    test_levels_db = np.arange(-50, 1, 2)  # -50dB to 0dB in 2dB steps

    print("\n  Transfer Curve Measurement:")
    print(f"    Threshold: {threshold_db}dB, Ratio: {ratio}:1")
    print(f"    Attack: {attack_ms}ms, Release: {release_ms}ms")

    input_rms_db = []
    output_rms_db = []

    for level_db in test_levels_db:
        host = CedarTestHost(sr)

        amplitude = db_to_linear(level_db)
        test_signal = gen_test_tone(freq, 0.5, sr, amplitude)

        # DYNAMICS_COMP: out = comp(in, threshold, ratio)
        _build_comp(host, threshold_db, ratio, attack_ms, release_ms)

        output = host.process_loaded(test_signal)

        # Measure RMS of steady-state portion (skip attack)
        steady_start = int(0.2 * sr)  # Skip first 200ms
        in_rms = np.sqrt(np.mean(test_signal[steady_start:] ** 2))
        out_rms = np.sqrt(np.mean(output[steady_start:] ** 2))

        in_db = linear_to_db(in_rms)
        out_db = linear_to_db(out_rms)

        input_rms_db.append(in_db)
        output_rms_db.append(out_db)

        results['transfer_curve'].append({
            'input_db': float(in_db),
            'output_db': float(out_db)
        })

    # Calculate expected transfer curve
    expected_output_db = []
    for in_db in input_rms_db:
        if in_db <= threshold_db:
            expected_output_db.append(in_db)
        else:
            # Above threshold: out = threshold + (in - threshold) / ratio
            expected = threshold_db + (in_db - threshold_db) / ratio
            expected_output_db.append(expected)

    # Calculate error
    errors = np.abs(np.array(output_rms_db) - np.array(expected_output_db))
    max_error = np.max(errors)
    avg_error = np.mean(errors)

    # Check gain reduction above threshold
    above_thresh_mask = np.array(input_rms_db) > threshold_db
    if np.any(above_thresh_mask):
        gr_errors = errors[above_thresh_mask]
        gr_max_error = np.max(gr_errors)
        gr_passed = gr_max_error < 3.0  # Allow 3dB tolerance for GR
    else:
        gr_passed = True
        gr_max_error = 0

    results['tests'].append({
        'name': 'Transfer curve',
        'max_error_db': float(max_error),
        'avg_error_db': float(avg_error),
        'gain_reduction_max_error': float(gr_max_error),
        'passed': gr_passed
    })

    status = "PASS" if gr_passed else "FAIL"
    print(f"\n    Max error: {max_error:.2f}dB")
    print(f"    Avg error: {avg_error:.2f}dB")
    print(f"    GR accuracy: {gr_max_error:.2f}dB [{status}]")

    # Test attack/release timing
    print("\n  Attack/Release Timing:")

    # Create signal with sudden level change for timing test
    timing_dur = 1.0
    timing_signal = np.zeros(int(timing_dur * sr), dtype=np.float32)
    # Low level for first 0.3s, then high level
    low_amp = db_to_linear(-40)
    high_amp = db_to_linear(-6)  # Well above threshold
    transition_sample = int(0.3 * sr)

    t_low = np.arange(transition_sample) / sr
    t_high = np.arange(int(timing_dur * sr) - transition_sample) / sr
    timing_signal[:transition_sample] = np.sin(2 * np.pi * freq * t_low) * low_amp
    timing_signal[transition_sample:] = np.sin(2 * np.pi * freq * t_high) * high_amp

    host2 = CedarTestHost(sr)
    _build_comp(host2, threshold_db, ratio, attack_ms, release_ms, state_name="comp2")

    timing_output = host2.process_loaded(timing_signal)

    # Measure envelope of output
    env_in = np.abs(timing_signal)
    env_out = np.abs(timing_output)

    # Visualization
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Transfer curve
    ax1 = axes[0, 0]
    ax1.plot(input_rms_db, input_rms_db, 'k--', alpha=0.3, label='Unity (no compression)')
    ax1.plot(input_rms_db, expected_output_db, 'g-', linewidth=2, label='Expected')
    ax1.plot(input_rms_db, output_rms_db, 'b.', markersize=8, label='Measured')
    ax1.axvline(threshold_db, color='red', linestyle=':', alpha=0.5, label=f'Threshold={threshold_db}dB')
    ax1.axhline(threshold_db, color='red', linestyle=':', alpha=0.5)
    ax1.set_xlabel('Input Level (dB)')
    ax1.set_ylabel('Output Level (dB)')
    ax1.set_title(f'Compressor Transfer Curve (Ratio {ratio}:1)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(-55, 5)
    ax1.set_ylim(-55, 5)

    # Error plot
    ax2 = axes[0, 1]
    ax2.plot(input_rms_db, errors, 'b-', linewidth=1)
    ax2.axhline(3.0, color='red', linestyle='--', alpha=0.5, label='3dB tolerance')
    ax2.axvline(threshold_db, color='orange', linestyle=':', alpha=0.5, label='Threshold')
    ax2.set_xlabel('Input Level (dB)')
    ax2.set_ylabel('Error (dB)')
    ax2.set_title('Transfer Curve Error')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    # Timing waveform
    ax3 = axes[1, 0]
    time_ms = np.arange(len(timing_signal)) / sr * 1000
    ax3.plot(time_ms, timing_signal, 'g-', linewidth=0.3, alpha=0.5, label='Input')
    ax3.plot(time_ms, timing_output, 'b-', linewidth=0.3, alpha=0.7, label='Output')
    ax3.axvline(transition_sample / sr * 1000, color='red', linestyle=':', alpha=0.7, label='Level change')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Amplitude')
    ax3.set_title('Attack/Release Response')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Gain reduction over time
    ax4 = axes[1, 1]
    # Calculate instantaneous GR (smoothed)
    window = int(0.01 * sr)  # 10ms window
    if window > 0:
        env_in_smooth = np.convolve(np.abs(timing_signal), np.ones(window)/window, mode='same')
        env_out_smooth = np.convolve(np.abs(timing_output), np.ones(window)/window, mode='same')
        gr_db = linear_to_db(env_out_smooth + 1e-10) - linear_to_db(env_in_smooth + 1e-10)
        ax4.plot(time_ms, gr_db, 'b-', linewidth=1)
        ax4.axhline(0, color='gray', linestyle='--', alpha=0.5)
        ax4.axvline(transition_sample / sr * 1000, color='red', linestyle=':', alpha=0.7)
    ax4.set_xlabel('Time (ms)')
    ax4.set_ylabel('Gain Reduction (dB)')
    ax4.set_title('Gain Reduction vs Time')
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'curve.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'curve.png')}")

    with open(os.path.join(OUT, 'curve.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'curve.json')}")

    return results


# =============================================================================
# ExtendedParams migration tests (prd-extended-params-migration §4.1)
# =============================================================================

def test_default_fallback_equivalence():
    """
    Verify the nullptr-fallback (no ExtendedParams init) is identical to an
    explicit-default init (attack=0.1ms, release=10ms).

    Pre-migration, comp always ran at inst.rate=0 → attack=0.1ms, release=10ms
    (no Akkado path ever set the rate field). The post-migration opcode must
    reproduce that exactly when ExtendedParams is absent.

    Acceptance: peak sample error < -60 dBFS between the two renders.
    """
    print("Test 2: comp default-fallback equivalence")
    print("=" * 60)

    sr = 48000
    transient = np.zeros(int(0.5 * sr), dtype=np.float32)
    t = np.arange(len(transient)) / sr
    transient[:] = np.sin(2 * np.pi * 800 * t) * db_to_linear(-3)

    host_fallback = CedarTestHost(sr)
    _build_comp(host_fallback, -20, 4.0, init_ext=False, state_name="comp_fb")
    out_fallback = host_fallback.process_loaded(transient)

    host_default = CedarTestHost(sr)
    _build_comp(host_default, -20, 4.0, attack_ms=0.1, release_ms=10.0,
                state_name="comp_def")
    out_default = host_default.process_loaded(transient)

    peak_err = float(np.max(np.abs(out_fallback - out_default)))
    peak_err_db = linear_to_db(peak_err + 1e-12)

    wav_path = os.path.join(OUT, "default_equivalence.wav")
    save_wav(wav_path, out_default, sr)
    print(f"  Saved {wav_path} - reference default-param render")
    print(f"  Peak error fallback vs default-init: {peak_err_db:.1f} dBFS")

    passed = peak_err_db < -60.0
    print(f"  {'✓ PASS' if passed else '✗ FAIL'}: nullptr fallback "
          f"{'matches' if passed else 'DIVERGES FROM'} explicit defaults")
    return passed


def test_attack_release_tunable():
    """
    Verify attack/release are now tunable via ExtendedParams.

    Drives a sudden -40dB→-3dB level jump (well above a -20dB threshold).
    A fast attack (0.1ms) clamps the transient overshoot sooner than a slow
    attack (100ms), so the slow-attack render must let through a measurably
    larger peak right after the jump.

    Acceptance: slow-attack post-transition peak > fast-attack peak by ≥ 1 dB.
    """
    print("Test 3: comp attack/release tunability")
    print("=" * 60)

    sr = 48000
    freq = 1000
    dur = 1.0
    sig = np.zeros(int(dur * sr), dtype=np.float32)
    trans = int(0.3 * sr)
    t_lo = np.arange(trans) / sr
    t_hi = np.arange(len(sig) - trans) / sr
    sig[:trans] = np.sin(2 * np.pi * freq * t_lo) * db_to_linear(-40)
    sig[trans:] = np.sin(2 * np.pi * freq * t_hi) * db_to_linear(-3)

    def render(attack_ms, name):
        host = CedarTestHost(sr)
        _build_comp(host, -20, 8.0, attack_ms=attack_ms, release_ms=100.0,
                    state_name=name)
        return host.process_loaded(sig)

    out_fast = render(0.1, "comp_fast")
    out_slow = render(100.0, "comp_slow")

    # Post-transition window: first 20ms after the level jump.
    win = slice(trans, trans + int(0.02 * sr))
    peak_fast = float(np.max(np.abs(out_fast[win])))
    peak_slow = float(np.max(np.abs(out_slow[win])))
    diff_db = linear_to_db(peak_slow + 1e-12) - linear_to_db(peak_fast + 1e-12)

    save_wav(os.path.join(OUT, "attack_fast.wav"), out_fast, sr)
    save_wav(os.path.join(OUT, "attack_slow.wav"), out_slow, sr)
    print(f"  Saved attack_fast.wav / attack_slow.wav - listen for transient clamp")
    print(f"  Fast-attack peak: {linear_to_db(peak_fast):.1f} dB, "
          f"slow-attack peak: {linear_to_db(peak_slow):.1f} dB")
    print(f"  Difference: {diff_db:.2f} dB (slow lets through more transient)")

    passed = diff_db >= 1.0
    print(f"  {'✓ PASS' if passed else '✗ FAIL'}: attack param "
          f"{'is tunable' if passed else 'has NO measurable effect'}")
    return passed


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar Compressor Quality Tests")
    print("=" * 60)
    print()

    test_compressor_ratio()
    print()
    test_default_fallback_equivalence()
    print()
    test_attack_release_tunable()

    print()
    print("=" * 60)
    print("Compressor tests complete.")
