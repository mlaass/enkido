"""
Limiter Quality Tests (Cedar Engine)
=====================================
Tests for DYNAMICS_LIMITER opcode.
Validates ceiling enforcement and peak prevention.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, db_to_linear, linear_to_db, save_wav
from visualize import save_figure

OUT = output_dir("op_limiter")


def gen_test_tone(freq, duration, sr, amplitude=1.0):
    """Generate a test sine tone."""
    t = np.arange(int(duration * sr)) / sr
    return (np.sin(2 * np.pi * freq * t) * amplitude).astype(np.float32)


def _build_limiter(host, ceiling_db, release_ms, lookahead_ms=0.0,
                   init_ext=True, state_name="limiter"):
    """Configure host with a DYNAMICS_LIMITER instruction.

    lookahead moved from the inst.rate boolean toggle to ExtendedParams<1>
    (prd-extended-params-migration §4.2). lookahead_ms=0 disables it. When
    init_ext is False the ExtendedParams state is left uninitialized to
    exercise the opcode's nullptr fallback (lookahead off).
    buf_in=0, buf_out=1.
    """
    buf_ceiling = host.set_param("ceiling", ceiling_db)
    buf_release = host.set_param("release", release_ms)
    state_id = cedar.hash(state_name) & 0xFFFF
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.DYNAMICS_LIMITER, 1, 0, buf_ceiling, buf_release, state_id
    )
    inst.rate = 0  # rate field no longer carries the lookahead toggle
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))
    host.vm.load_program(host.program)
    if init_ext:
        host.vm.init_extended_params(
            cedar.ext_params_state_id(state_id),
            np.array([lookahead_ms], dtype=np.float32),
            np.array([0xFFFF], dtype=np.uint16),
            1,
        )
    return state_id


# =============================================================================
# DYNAMICS_LIMITER Test - Ceiling and Peak Prevention
# =============================================================================

def test_limiter_ceiling():
    """
    Test limiter prevents output from exceeding ceiling.
    - Input: sine with peaks at +6dB
    - Settings: ceiling=-3dB
    - Output should never exceed ceiling
    """
    print("\nTest 2: DYNAMICS_LIMITER (Limiter) Ceiling")
    print("=" * 60)

    sr = 48000

    # Limiter settings
    ceiling_db = -3
    ceiling_linear = db_to_linear(ceiling_db)
    release_ms = 100

    results = {'sample_rate': sr, 'tests': []}

    # Test with various input levels above ceiling
    test_cases = [
        (-6, "6dB above ceiling"),
        (0, "3dB above ceiling"),
        (-3, "At ceiling"),
        (-10, "Below ceiling"),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for idx, (input_db, name) in enumerate(test_cases):
        print(f"\n  {name} (input peak: {input_db}dB, ceiling: {ceiling_db}dB):")

        host = CedarTestHost(sr)

        # Generate test signal
        amplitude = db_to_linear(input_db)
        duration = 0.5
        freq = 1000
        test_signal = gen_test_tone(freq, duration, sr, amplitude)

        # Set limiter parameters
        buf_ceiling = host.set_param("ceiling", ceiling_db)
        buf_release = host.set_param("release", release_ms)
        buf_in = 0
        buf_out = 1

        # DYNAMICS_LIMITER: out = limiter(in, ceiling, release)
        host.load_instruction(
            cedar.Instruction.make_ternary(
                cedar.Opcode.DYNAMICS_LIMITER, buf_out, buf_in, buf_ceiling, buf_release,
                cedar.hash("limiter") & 0xFFFF
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        output = host.process(test_signal)

        # Measure peak output
        peak_out = np.max(np.abs(output))
        peak_out_db = linear_to_db(peak_out)

        # Check if ceiling is respected
        # Allow small overshoot (0.5dB) for non-lookahead limiters
        overshoot_tolerance = 0.5
        ceiling_respected = peak_out_db <= ceiling_db + overshoot_tolerance

        # Calculate actual overshoot
        overshoot_db = max(0, peak_out_db - ceiling_db)

        test_result = {
            'name': name,
            'input_peak_db': input_db,
            'output_peak_db': float(peak_out_db),
            'overshoot_db': float(overshoot_db),
            'ceiling_respected': ceiling_respected
        }
        results['tests'].append(test_result)

        status = "PASS" if ceiling_respected else "FAIL"
        print(f"    Input peak:  {input_db:.1f}dB")
        print(f"    Output peak: {peak_out_db:.2f}dB")
        print(f"    Overshoot:   {overshoot_db:.2f}dB [{status}]")

        # Plot
        ax = axes[idx // 2, idx % 2]
        time_ms = np.arange(len(output)) / sr * 1000
        ax.plot(time_ms, test_signal, 'g-', linewidth=0.5, alpha=0.5, label='Input')
        ax.plot(time_ms, output, 'b-', linewidth=0.5, alpha=0.8, label='Output')
        ax.axhline(ceiling_linear, color='red', linestyle='--', alpha=0.7,
                   label=f'Ceiling={ceiling_db}dB')
        ax.axhline(-ceiling_linear, color='red', linestyle='--', alpha=0.7)
        ax.set_xlabel('Time (ms)')
        ax.set_ylabel('Amplitude')
        ax.set_title(f'{name}')
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, 'ceiling.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'ceiling.png')}")

    # Test transient response
    print("\n  Transient Response Test:")

    host2 = CedarTestHost(sr)

    # Create signal with sudden transient
    transient_dur = 0.5
    transient_signal = np.zeros(int(transient_dur * sr), dtype=np.float32)
    t = np.arange(len(transient_signal)) / sr

    # Low level background with high transient peaks
    background_amp = db_to_linear(-20)
    peak_amp = db_to_linear(0)  # 3dB above ceiling

    # Background tone
    transient_signal = np.sin(2 * np.pi * 1000 * t) * background_amp

    # Add transient peaks
    for peak_time in [0.1, 0.2, 0.3, 0.4]:
        peak_sample = int(peak_time * sr)
        # Sharp attack, slow decay transient
        env_len = int(0.01 * sr)
        for i in range(env_len):
            if peak_sample + i < len(transient_signal):
                env = np.exp(-i / (env_len / 5))
                transient_signal[peak_sample + i] += peak_amp * env * np.sin(2 * np.pi * 2000 * (i / sr))

    buf_ceiling2 = host2.set_param("ceiling", ceiling_db)
    buf_release2 = host2.set_param("release", release_ms)
    buf_out2 = 1

    host2.load_instruction(
        cedar.Instruction.make_ternary(
            cedar.Opcode.DYNAMICS_LIMITER, buf_out2, 0, buf_ceiling2, buf_release2,
            cedar.hash("limiter2") & 0xFFFF
        )
    )
    host2.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2)
    )

    transient_output = host2.process(transient_signal)

    peak_transient_out = np.max(np.abs(transient_output))
    peak_transient_db = linear_to_db(peak_transient_out)
    transient_passed = peak_transient_db <= ceiling_db + 1.0  # Allow 1dB for transients

    results['tests'].append({
        'name': 'Transient handling',
        'peak_transient_db': float(peak_transient_db),
        'passed': transient_passed
    })

    print(f"    Peak with transients: {peak_transient_db:.2f}dB")
    print(f"    Transient test: {'PASS' if transient_passed else 'FAIL'}")

    with open(os.path.join(OUT, 'ceiling.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'ceiling.json')}")

    return results


# =============================================================================
# ExtendedParams migration tests (prd-extended-params-migration §4.2)
# =============================================================================

def test_default_fallback_equivalence():
    """
    Verify the nullptr-fallback (no ExtendedParams init) is identical to an
    explicit lookahead=0 init.

    Pre-migration, limiter always ran at inst.rate=0 → lookahead off (no
    Akkado path ever set the rate field). The post-migration opcode must
    reproduce that exactly when ExtendedParams is absent.

    Acceptance: peak sample error < -60 dBFS between the two renders.
    """
    print("\nTest: limiter default-fallback equivalence")
    print("=" * 60)

    sr = 48000
    rng = np.random.default_rng(0xF00D)
    sig = rng.standard_normal(int(0.5 * sr)).astype(np.float32) * 0.7

    host_fb = CedarTestHost(sr)
    _build_limiter(host_fb, -3, 100, init_ext=False, state_name="lim_fb")
    out_fb = host_fb.process_loaded(sig)

    host_def = CedarTestHost(sr)
    _build_limiter(host_def, -3, 100, lookahead_ms=0.0, state_name="lim_def")
    out_def = host_def.process_loaded(sig)

    peak_err = float(np.max(np.abs(out_fb - out_def)))
    peak_err_db = linear_to_db(peak_err + 1e-12)
    print(f"  Peak error fallback vs lookahead=0 init: {peak_err_db:.1f} dBFS")

    passed = peak_err_db < -60.0
    print(f"  {'✓ PASS' if passed else '✗ FAIL'}: nullptr fallback "
          f"{'matches' if passed else 'DIVERGES FROM'} explicit lookahead=0")
    return passed


def test_lookahead_tunable():
    """
    Verify lookahead is now tunable via ExtendedParams.

    With lookahead enabled the limiter analyzes (and outputs) a delayed copy
    of the signal — so the lookahead render is time-shifted relative to the
    no-lookahead render. The shift equals the configured lookahead time,
    capped at ~1ms (47 samples at 48kHz) by the fixed lookahead buffer.

    Acceptance: cross-correlation lag between lookahead=0 and lookahead=1ms
    renders is within ±5 samples of the expected 47-sample delay.
    """
    print("\nTest: limiter lookahead tunability")
    print("=" * 60)

    sr = 48000
    rng = np.random.default_rng(0x11AA)
    sig = rng.standard_normal(int(0.5 * sr)).astype(np.float32) * 0.6

    host_off = CedarTestHost(sr)
    _build_limiter(host_off, -3, 100, lookahead_ms=0.0, state_name="lim_off")
    out_off = host_off.process_loaded(sig)

    host_on = CedarTestHost(sr)
    _build_limiter(host_on, -3, 100, lookahead_ms=1.0, state_name="lim_on")
    out_on = host_on.process_loaded(sig)

    save_wav(os.path.join(OUT, "lookahead_off.wav"), out_off, sr)
    save_wav(os.path.join(OUT, "lookahead_on.wav"), out_on, sr)
    print("  Saved lookahead_off.wav / lookahead_on.wav")

    # Cross-correlation lag: positive = out_on lags out_off.
    xcorr = np.correlate(out_on, out_off, mode="full")
    lag = int(np.argmax(np.abs(xcorr))) - (len(out_off) - 1)
    expected = 47  # 1ms clamps to LOOKAHEAD_SAMPLES-1
    print(f"  Measured lookahead delay: {lag} samples (expected ~{expected})")

    passed = abs(lag - expected) <= 5
    print(f"  {'✓ PASS' if passed else '✗ FAIL'}: lookahead param "
          f"{'is tunable' if passed else 'has NO measurable effect'}")
    return passed


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar Limiter Quality Tests")
    print("=" * 60)
    print()

    test_limiter_ceiling()
    test_default_fallback_equivalence()
    test_lookahead_tunable()

    print()
    print("=" * 60)
    print("Limiter tests complete.")
