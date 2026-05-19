"""
Test: EFFECT_COMB (Feedback Comb Filter)
=========================================
Tests comb filter resonance, feedback polarity, and damping.

Expected behavior:
- Resonance peaks at harmonics of 1/delay_time
- Positive feedback: peaks at fundamental and harmonics
- Negative feedback: notch at fundamental, peaks at odd harmonics
- Damping narrows bandwidth at higher frequencies

If this test fails, check the implementation in cedar/include/cedar/opcodes/modulation.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_white_noise, gen_impulse, save_wav
from visualize import save_figure
from filter_helpers import get_bode_data

OUT = output_dir("op_comb")


def _build_comb(host, delay_ms, feedback, damping=0.0, dry=0.0, wet=1.0,
                init_ext=True, state_name="comb"):
    """Configure host with an EFFECT_COMB instruction.

    damping moved from inst.rate bit-packing to ExtendedParams<1>
    (prd-extended-params-migration §4.7). When init_ext is False the
    ExtendedParams state is left uninitialized to exercise the opcode's
    nullptr fallback (damping=0). buf_in=0, buf_out=1.
    """
    buf_time = host.set_param("c_time", delay_ms)
    buf_fb = host.set_param("c_fb", feedback)
    buf_dry = host.set_param("c_dry", dry)
    buf_wet = host.set_param("c_wet", wet)
    state_id = cedar.hash(state_name) & 0xFFFF
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.EFFECT_COMB, 1, 0, buf_time, buf_fb, buf_dry, buf_wet,
        state_id
    )
    inst.rate = 0  # rate field no longer carries damping
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))
    host.vm.load_program(host.program)
    if init_ext:
        host.vm.init_extended_params(
            cedar.ext_params_state_id(state_id),
            np.array([damping], dtype=np.float32),
            np.array([0xFFFF], dtype=np.uint16),
            1,
        )
    return state_id


def test_resonance_frequency():
    """
    Test resonance peaks at harmonics of 1/delay_time.

    Expected:
    - Feed noise through comb filter
    - Peaks in spectrum at f0 = 1/delay_time, 2*f0, 3*f0, ...
    """
    print("Test: EFFECT_COMB Resonance Frequency")

    sr = 48000
    duration = 1.0
    delay_ms = 5.0  # → f0 = 1/0.005 = 200 Hz
    expected_f0 = 1000.0 / delay_ms  # 200 Hz

    np.random.seed(42)
    noise = gen_white_noise(duration, sr)

    host = CedarTestHost(sr)
    # Comb is Category A (default dry=1, wet=0.5); the legacy test measures
    # the raw comb spectrum, so force dry=0, wet=1.
    _build_comb(host, delay_ms, 0.8, damping=0.0)

    output = host.process_loaded(noise)

    # Save WAV
    wav_path = os.path.join(OUT, "comb_noise.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for pitched resonance at {expected_f0}Hz")

    # FFT analysis
    fft_size = 8192
    steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
    freqs = np.fft.rfftfreq(fft_size, 1 / sr)
    spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
    spectrum_db = 20 * np.log10(spectrum + 1e-10)

    # Find peaks
    peaks_found = []
    for h in range(1, 10):
        expected = expected_f0 * h
        if expected > sr / 2:
            break
        idx = np.argmin(np.abs(freqs - expected))
        # Search local window for actual peak
        window = 10
        start = max(0, idx - window)
        end = min(len(spectrum_db), idx + window)
        local_peak = start + np.argmax(spectrum_db[start:end])
        actual_freq = freqs[local_peak]
        error = abs(actual_freq - expected)
        peaks_found.append((h, expected, actual_freq, error))
        print(f"  Harmonic {h}: expected={expected:.0f}Hz, found={actual_freq:.0f}Hz, error={error:.1f}Hz")

    # Check first few harmonics are close
    if peaks_found and all(p[3] < 20 for p in peaks_found[:3]):
        print("  ✓ PASS: Resonance peaks at expected frequencies")
    else:
        print("  ✗ FAIL: Resonance peaks not at expected frequencies")

    # Plot
    fig, ax = plt.subplots(figsize=(14, 6))
    ax.plot(freqs, spectrum_db, linewidth=0.5)
    for h, expected, actual, _ in peaks_found:
        ax.axvline(expected, color='red', linestyle='--', alpha=0.3, linewidth=0.5)
    ax.set_title(f'Comb Filter (delay={delay_ms}ms, fb=0.8, f0={expected_f0}Hz)')
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_xlim(0, 5000)
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "comb_resonance.png"))
    print(f"  Saved {os.path.join(OUT, 'comb_resonance.png')}")


def test_feedback_sign():
    """
    Test positive vs negative feedback.

    Expected:
    - Positive feedback: peaks at f0, 2*f0, 3*f0 (all harmonics)
    - Negative feedback: notch at f0, peaks at 1.5*f0, 2.5*f0 (odd harmonics shifted)
    """
    print("Test: EFFECT_COMB Feedback Sign")

    sr = 48000
    duration = 1.0
    delay_ms = 5.0
    expected_f0 = 1000.0 / delay_ms

    np.random.seed(42)
    noise = gen_white_noise(duration, sr)

    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle("Comb Filter: Positive vs Negative Feedback")

    for fb, label, ax in [(0.8, "Positive fb=0.8", axes[0]),
                           (-0.8, "Negative fb=-0.8", axes[1])]:
        host = CedarTestHost(sr)
        _build_comb(host, delay_ms, fb, damping=0.0, state_name="comb_sign")

        output = host.process_loaded(noise)

        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        ax.plot(freqs, spectrum_db, linewidth=0.5)
        for h in range(1, 10):
            ax.axvline(expected_f0 * h, color='red', linestyle=':', alpha=0.3)
        ax.set_title(label)
        ax.set_xlim(0, 3000)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "comb_feedback_sign.png"))
    print(f"  Saved {os.path.join(OUT, 'comb_feedback_sign.png')}")


def _hf_energy_db(signal, sr, cutoff=4000.0):
    """Total spectral energy above `cutoff` Hz, in dB."""
    spec = np.abs(np.fft.rfft(signal))
    freqs = np.fft.rfftfreq(len(signal), 1.0 / sr)
    hf = spec[freqs >= cutoff]
    return 10.0 * np.log10(np.sum(hf ** 2) + 1e-12)


def test_damping():
    """
    Test damping parameter rolls off high-frequency content of the comb tail.

    Damping is a one-pole lowpass in the comb feedback path. The migration
    (prd-extended-params-migration §4.7) moves it from inst.rate bit-packing
    to ExtendedParams<1>. Higher damping → less high-frequency energy in the
    resonant output. High feedback (0.92) rings the tail through many loop
    passes so the per-pass lowpass compounds into a clearly audible rolloff.

    Acceptance: HF energy (>4kHz) drops by ≥ 6 dB from damping=0.0 to
    damping=0.9.
    """
    print("Test: EFFECT_COMB Damping")

    sr = 48000
    duration = 1.0
    delay_ms = 5.0

    np.random.seed(42)
    noise = gen_white_noise(duration, sr)

    damp_values = [0.0, 0.3, 0.6, 0.9]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Comb Filter Damping Effect")
    hf_by_damp = {}

    for damp, ax in zip(damp_values, axes.flat):
        host = CedarTestHost(sr)
        _build_comb(host, delay_ms, 0.92, damping=damp,
                    state_name=f"comb_damp_{damp}")
        output = host.process_loaded(noise)

        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)
        hf_by_damp[damp] = _hf_energy_db(steady, sr)

        ax.plot(freqs, spectrum_db, linewidth=0.5)
        ax.set_title(f'Damping={damp}')
        ax.set_xlim(0, sr / 2)
        ax.set_ylim(-60, 0)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.grid(True, alpha=0.3)

        print(f"  Damping {damp}: HF energy = {hf_by_damp[damp]:.1f} dB")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "comb_damping.png"))
    print(f"  Saved {os.path.join(OUT, 'comb_damping.png')}")

    drop_db = hf_by_damp[0.0] - hf_by_damp[0.9]
    print(f"  HF rolloff from damping=0.0 → 0.9: {drop_db:.2f} dB")
    if drop_db >= 6.0:
        print("  ✓ PASS: damping param is tunable")
        return True
    print("  ✗ FAIL: damping param has NO measurable effect")
    return False


def test_default_fallback_equivalence():
    """
    Verify the nullptr-fallback (no ExtendedParams init) is identical to an
    explicit damping=0 init.

    Pre-migration, comb damping came from inst.rate=0 → 0.0. The
    post-migration opcode must reproduce that when ExtendedParams is absent.

    Acceptance: peak sample error < -60 dBFS between the two renders.
    """
    print("Test: EFFECT_COMB default-fallback equivalence")
    sr = 48000
    np.random.seed(7)
    noise = gen_white_noise(1.0, sr)

    host_fb = CedarTestHost(sr)
    _build_comb(host_fb, 5.0, 0.8, init_ext=False, state_name="comb_fb")
    out_fb = host_fb.process_loaded(noise)

    host_def = CedarTestHost(sr)
    _build_comb(host_def, 5.0, 0.8, damping=0.0, state_name="comb_def")
    out_def = host_def.process_loaded(noise)

    peak_err = float(np.max(np.abs(out_fb - out_def)))
    peak_err_db = 20.0 * np.log10(peak_err + 1e-12)
    print(f"  Peak error fallback vs damping=0 init: {peak_err_db:.1f} dBFS")
    if peak_err_db < -60.0:
        print("  ✓ PASS: nullptr fallback matches explicit damping=0")
        return True
    print("  ✗ FAIL: nullptr fallback DIVERGES from explicit damping=0")
    return False


if __name__ == "__main__":
    test_resonance_frequency()
    test_feedback_sign()
    test_damping()
    test_default_fallback_equivalence()
