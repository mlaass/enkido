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
    buf_in = 0
    buf_time = host.set_param("time", delay_ms)
    buf_fb = host.set_param("feedback", 0.8)
    # Post-unified-drywet: comb is Category A (default dry=1, wet=0.5). The
    # legacy test measures the raw comb spectrum, so force dry=0, wet=1.
    buf_dry = host.set_param("dry", 0.0)
    buf_wet = host.set_param("wet", 1.0)

    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.EFFECT_COMB, 1, buf_in, buf_time, buf_fb, buf_dry, buf_wet,
        cedar.hash("comb") & 0xFFFF
    )
    inst.rate = 0  # No damping
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(noise)

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
        buf_in = 0
        buf_time = host.set_param("time", delay_ms)
        buf_fb = host.set_param("feedback", fb)
        buf_dry = host.set_param("dry", 0.0)
        buf_wet = host.set_param("wet", 1.0)

        inst = cedar.Instruction.make_quinary(
            cedar.Opcode.EFFECT_COMB, 1, buf_in, buf_time, buf_fb, buf_dry, buf_wet,
            cedar.hash("comb_sign") & 0xFFFF
        )
        inst.rate = 0
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(noise)

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


def test_damping():
    """
    Test damping parameter narrows HF resonance bandwidth.

    Expected:
    - Higher damping → narrower resonance peaks at high frequencies
    - Low-frequency peaks remain relatively unchanged
    """
    print("Test: EFFECT_COMB Damping")

    sr = 48000
    duration = 1.0
    delay_ms = 5.0

    np.random.seed(42)
    noise = gen_white_noise(duration, sr)

    damp_values = [0, 64, 128, 200]  # rate field 0-255

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Comb Filter Damping Effect")

    for damp, ax in zip(damp_values, axes.flat):
        host = CedarTestHost(sr)
        buf_in = 0
        buf_time = host.set_param("time", delay_ms)
        buf_fb = host.set_param("feedback", 0.8)
        buf_dry = host.set_param("dry", 0.0)
        buf_wet = host.set_param("wet", 1.0)

        inst = cedar.Instruction.make_quinary(
            cedar.Opcode.EFFECT_COMB, 1, buf_in, buf_time, buf_fb, buf_dry, buf_wet,
            cedar.hash("comb_damp") & 0xFFFF
        )
        inst.rate = damp
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(noise)

        fft_size = 8192
        steady = output[int(0.2 * sr):int(0.2 * sr) + fft_size]
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        ax.plot(freqs, spectrum_db, linewidth=0.5)
        ax.set_title(f'Damping={damp}/255')
        ax.set_xlim(0, sr / 2)
        ax.set_ylim(-60, 0)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.grid(True, alpha=0.3)

        print(f"  Damping {damp}/255: processed")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "comb_damping.png"))
    print(f"  Saved {os.path.join(OUT, 'comb_damping.png')}")


if __name__ == "__main__":
    test_resonance_frequency()
    test_feedback_sign()
    test_damping()
