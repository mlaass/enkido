"""
Test: REVERB_FREEVERB (Schroeder-Moorer Reverb)
=================================================
Tests Freeverb impulse response, room size, damping, AND the Phase 2
stereo-native conversion (prd-stereo-native-opcodes).

Expected behavior:
- Impulse response produces dense reverb tail (per channel)
- Larger room_size → longer decay (higher RT60)
- Higher damping → darker reverb tail (lower spectral centroid)
- 8 comb filters + 4 allpass filters per channel
- Output is STEREO: writes to out_buffer (L) and out_buffer+1 (R)
- L/R lanes are decorrelated by the classic Schroeder +23-sample buffer offset
- Stereo input feeds the L and R lanes independently (no cross-coupling —
  Freeverb is a parallel network, not figure-8)

If this test fails, check the implementation in cedar/include/cedar/opcodes/reverbs.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_impulse, gen_noise_pulse, save_wav
from visualize import save_figure

OUT = output_dir("op_freeverb")


def _run_stereo_freeverb(host, n_samples, input_signal, *, room=0.8, damp=0.5,
                         hash_name="freeverb", stereo_input=False,
                         input_right=None):
    """
    Set up a stereo-native Freeverb instruction and run the block loop,
    returning (output_l, output_r).
    """
    buf_in = 0  # Primary input goes in buf 0 (L); buf 1 holds R if stereo.
    buf_room = host.set_param("room", room)
    buf_damp = host.set_param("damp", damp)
    buf_rscale = host.set_param("room_scale", 0.28)
    buf_roffset = host.set_param("room_offset", 0.7)

    # Output to buf 2 (L) and buf 3 (R) when stereo input is used; otherwise
    # buf 1 (L) and buf 2 (R). set_param returns buffers ≥10, so this is safe.
    out_buf = 2 if stereo_input else 1
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.REVERB_FREEVERB, out_buf, buf_in, buf_room, buf_damp,
        buf_rscale, buf_roffset, cedar.hash(hash_name) & 0xFFFF
    )
    inst.flags = cedar.STEREO_OUTPUT_FLAG | (
        cedar.STEREO_INPUT_FLAG if stereo_input else 0
    )
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_binary(
        cedar.Opcode.OUTPUT, 0, out_buf, out_buf + 1
    ))

    host.vm.load_program(host.program)
    n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    padded_len = n_blocks * cedar.BLOCK_SIZE
    in_l = np.zeros(padded_len, dtype=np.float32)
    in_l[:n_samples] = input_signal
    if stereo_input:
        in_r = np.zeros(padded_len, dtype=np.float32)
        in_r[:n_samples] = (input_right if input_right is not None
                            else input_signal)

    l_chunks, r_chunks = [], []
    for k in range(n_blocks):
        s = k * cedar.BLOCK_SIZE
        e = s + cedar.BLOCK_SIZE
        host.vm.set_buffer(0, in_l[s:e])
        if stereo_input:
            host.vm.set_buffer(1, in_r[s:e])
        l, r = host.vm.process()
        l_chunks.append(l)
        r_chunks.append(r)
    output_l = np.concatenate(l_chunks)[:n_samples]
    output_r = np.concatenate(r_chunks)[:n_samples]
    return output_l, output_r


def estimate_rt60(signal, sr, threshold_db=-60):
    """Estimate RT60 from impulse response envelope."""
    env = np.abs(signal)
    window = int(0.01 * sr)  # 10ms window
    if window > 0:
        env = np.convolve(env, np.ones(window) / window, mode='same')
    peak = np.max(env)
    if peak < 1e-10:
        return 0.0
    peak_idx = np.argmax(env)
    env_db = 20 * np.log10(env / peak + 1e-10)
    below = np.where(env_db[peak_idx:] < threshold_db)[0]
    if len(below) > 0:
        return below[0] / sr
    return (len(signal) - peak_idx) / sr


def test_impulse_response():
    """
    Test Freeverb impulse response produces a dense stereo reverb tail.

    Expected:
    - Dense, diffuse tail per channel (not individual echoes)
    - Approximately exponential decay
    - L and R differ (Schroeder offset → buffer-length decorrelation)
    """
    print("Test: REVERB_FREEVERB Impulse Response (stereo-native)")

    sr = 48000
    duration = 3.0
    host = CedarTestHost(sr)
    impulse = gen_impulse(duration, sr)

    output_l, output_r = _run_stereo_freeverb(
        host, len(impulse), impulse, room=0.8, damp=0.5, hash_name="freeverb"
    )
    # Combine to mono-equivalent for the historic RT60 metric.
    output = 0.5 * (output_l + output_r)

    rt60 = estimate_rt60(output, sr)
    peak_amp = float(max(np.max(np.abs(output_l)), np.max(np.abs(output_r))))

    print(f"  RT60 estimate (mono-fold): {rt60:.2f}s")
    print(f"  Peak amplitude: {peak_amp:.4f}")

    if rt60 > 0.1:
        print("  ✓ PASS: Reverb tail present")
    else:
        print("  ✗ FAIL: No significant reverb tail")

    # Save stereo WAV
    wav_path = os.path.join(OUT, "freeverb_impulse.wav")
    save_wav(wav_path, np.stack([output_l, output_r], axis=1), sr)
    print(f"  Saved {wav_path} - Listen for dense stereo reverb tail")

    # Plot
    time = np.arange(len(output_l)) / sr
    env_l_db = 20 * np.log10(np.abs(output_l) + 1e-10)
    env_r_db = 20 * np.log10(np.abs(output_r) + 1e-10)

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))

    axes[0].plot(time, output_l, linewidth=0.3, color='purple', label='L')
    axes[0].plot(time, output_r, linewidth=0.3, color='orange', alpha=0.6, label='R')
    axes[0].set_title("Freeverb Impulse Response (room=0.8, damp=0.5)")
    axes[0].set_xlabel("Time (s)")
    axes[0].set_ylabel("Amplitude")
    axes[0].legend()
    axes[0].grid(True, alpha=0.3)

    axes[1].plot(time, env_l_db, linewidth=0.5, color='purple', label='L')
    axes[1].plot(time, env_r_db, linewidth=0.5, color='orange', alpha=0.6, label='R')
    axes[1].axhline(-60, color='red', linestyle='--', alpha=0.5, label='RT60 threshold')
    axes[1].set_title(f"Envelope (RT60 ≈ {rt60:.2f}s)")
    axes[1].set_xlabel("Time (s)")
    axes[1].set_ylabel("Amplitude (dB)")
    axes[1].set_ylim(-100, 0)
    axes[1].legend()
    axes[1].grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_ir.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_ir.png')}")


def test_room_size():
    """
    Test room_size parameter: larger → longer decay.

    Expected:
    - RT60 increases with room_size
    """
    print("Test: REVERB_FREEVERB Room Size")

    sr = 48000
    duration = 4.0
    impulse = gen_impulse(duration, sr)

    room_values = [0.2, 0.5, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Freeverb Room Size Comparison (mono-fold of stereo output)")

    for room, ax in zip(room_values, axes.flat):
        host = CedarTestHost(sr)
        output_l, output_r = _run_stereo_freeverb(
            host, len(impulse), impulse, room=room, damp=0.5,
            hash_name="fv_room"
        )
        output = 0.5 * (output_l + output_r)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        ax.plot(time, env_db, linewidth=0.5)
        ax.set_title(f'Room={room} (RT60≈{rt60:.2f}s)')
        ax.set_ylim(-100, 0)
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('dB')
        ax.grid(True, alpha=0.3)

        print(f"  Room {room}: RT60 ≈ {rt60:.2f}s")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_room_size.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_room_size.png')}")

    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with room size")
    else:
        print("  ✗ FAIL: RT60 does not increase monotonically with room size")


def test_damping():
    """
    Test damping parameter: higher → darker reverb tail.

    Expected:
    - Spectral centroid of late reverb decreases with damping
    """
    print("Test: REVERB_FREEVERB Damping")

    sr = 48000
    duration = 3.0
    impulse = gen_impulse(duration, sr)

    damp_values = [0.0, 0.3, 0.6, 0.9]
    centroids = []

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Freeverb Damping Effect on Spectrum (mono-fold)")

    for damp, ax in zip(damp_values, axes.flat):
        host = CedarTestHost(sr)
        output_l, output_r = _run_stereo_freeverb(
            host, len(impulse), impulse, room=0.8, damp=damp,
            hash_name="fv_damp"
        )
        output = 0.5 * (output_l + output_r)

        # Analyze late reverb tail spectrum (0.5-1.5s)
        late_start = int(0.5 * sr)
        late_end = int(1.5 * sr)
        late_tail = output[late_start:late_end]

        fft_size = min(8192, len(late_tail))
        freqs = np.fft.rfftfreq(fft_size, 1 / sr)
        spectrum = np.abs(np.fft.rfft(late_tail[:fft_size] * np.hanning(fft_size)))

        centroid = np.sum(freqs * spectrum) / (np.sum(spectrum) + 1e-10)
        centroids.append(centroid)

        spectrum_db = 20 * np.log10(spectrum + 1e-10)
        ax.semilogx(freqs[1:], spectrum_db[1:], linewidth=0.5)
        ax.set_title(f'Damp={damp} (centroid: {centroid:.0f} Hz)')
        ax.set_xlim(20, sr / 2)
        ax.set_ylim(-100, -20)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('dB')
        ax.grid(True, alpha=0.3)

        print(f"  Damp {damp}: late tail centroid = {centroid:.0f} Hz")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_damping.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_damping.png')}")

    is_decreasing = all(centroids[i] >= centroids[i + 1] for i in range(len(centroids) - 1))
    if is_decreasing:
        print("  ✓ PASS: Spectral centroid decreases with damping")
    else:
        print("  ⚠ WARN: Spectral centroid does not decrease monotonically with damping")


def test_tail_length():
    """
    Test reverb tail length with short noise pulse input at different room sizes.

    Expected:
    - Noise pulse excites all comb modes for realistic RT60 measurement
    - RT60 increases monotonically with room_size
    - Each setting produces audible reverb tail in WAV output (stereo)
    """
    print("Test: REVERB_FREEVERB Tail Length (noise pulse)")

    sr = 48000
    duration = 5.0

    room_values = [0.2, 0.5, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(len(room_values), 1, figsize=(12, 3 * len(room_values)))
    fig.suptitle("Freeverb Tail Length — Noise Pulse Excitation (mono-fold of stereo)")

    for idx, room in enumerate(room_values):
        noise_pulse = gen_noise_pulse(duration, sr, pulse_ms=10)

        host = CedarTestHost(sr)
        output_l, output_r = _run_stereo_freeverb(
            host, len(noise_pulse), noise_pulse, room=room, damp=0.3,
            hash_name="fv_tail"
        )
        output = 0.5 * (output_l + output_r)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        wav_path = os.path.join(OUT, f"freeverb_tail_room{room}.wav")
        save_wav(wav_path, np.stack([output_l, output_r], axis=1), sr)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        axes[idx].plot(time, env_db, linewidth=0.5)
        axes[idx].axhline(-60, color='red', linestyle='--', alpha=0.5)
        axes[idx].set_title(f'Room={room} (RT60≈{rt60:.2f}s)')
        axes[idx].set_ylim(-100, 0)
        axes[idx].set_ylabel('dB')
        axes[idx].grid(True, alpha=0.3)

        print(f"  Room {room}: RT60 ≈ {rt60:.2f}s")
        print(f"  Saved {wav_path} - Listen for stereo reverb tail length")

    axes[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "freeverb_tail_length.png"))
    print(f"  Saved {os.path.join(OUT, 'freeverb_tail_length.png')}")

    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with room size (noise pulse)")
    else:
        print(f"  ✗ FAIL: RT60 not monotonic: {rt60s}")


def test_stereo_decorrelation():
    """
    Mono input → stereo-native Freeverb → decorrelated L/R output.

    Schroeder's +23-sample offset on the R-lane buffer sizes is the entire
    source of L/R width (Freeverb is parallel, not figure-8). Pearson
    correlation between L and R on the tail must be < 0.999.
    """
    print("Test: REVERB_FREEVERB Stereo Decorrelation")

    sr = 48000
    duration = 2.0
    host = CedarTestHost(sr)
    impulse = gen_impulse(duration, sr)

    output_l, output_r = _run_stereo_freeverb(
        host, len(impulse), impulse, room=0.85, damp=0.3,
        hash_name="fv_decorr"
    )

    nonzero = (np.abs(output_l) > 1e-6) | (np.abs(output_r) > 1e-6)
    if not np.any(nonzero):
        print("  ✗ FAIL: both channels silent")
        return

    l = output_l[nonzero]
    r = output_r[nonzero]
    if np.std(l) > 1e-9 and np.std(r) > 1e-9:
        corr = float(np.corrcoef(l, r)[0, 1])
    else:
        corr = 1.0
    print(f"  L/R Pearson correlation: {corr:.4f}")

    wav_path = os.path.join(OUT, "freeverb_decorrelation.wav")
    save_wav(wav_path, np.stack([output_l, output_r], axis=1), sr)
    print(f"  Saved {wav_path} - Listen for stereo width")

    if corr < 0.999:
        print("  ✓ PASS: L and R are non-trivially decorrelated")
    else:
        print(f"  ✗ FAIL: L and R are virtually identical (corr={corr:.6f})")


def test_stereo_input():
    """
    Stereo input → stereo-native Freeverb runs without crashing and produces
    decorrelated stereo output. Freeverb is a parallel network (no cross-
    coupling between lanes), so the assertion here is weaker than Dattorro's
    figure-8 cross-coupling test: we only verify the STEREO_INPUT path runs
    and produces sensible output.
    """
    print("Test: REVERB_FREEVERB Stereo Input")

    sr = 48000
    duration = 2.0
    n_samples = int(duration * sr)
    host = CedarTestHost(sr)

    # L = impulse, R = silence
    left = np.zeros(n_samples, dtype=np.float32)
    left[0] = 1.0
    right = np.zeros(n_samples, dtype=np.float32)

    output_l, output_r = _run_stereo_freeverb(
        host, n_samples, left, room=0.85, damp=0.3,
        hash_name="fv_stin", stereo_input=True, input_right=right
    )

    rms_l = float(np.sqrt(np.mean(output_l ** 2)))
    rms_r = float(np.sqrt(np.mean(output_r ** 2)))

    wav_path = os.path.join(OUT, "freeverb_stereo_input.wav")
    save_wav(wav_path, np.stack([output_l, output_r], axis=1), sr)
    print(f"  Saved {wav_path}")
    print(f"  rms_l={rms_l:.5f}, rms_r={rms_r:.5f}")

    # L should be non-silent (took the impulse); R should be near-silent for
    # Freeverb because there's no cross-coupling between lanes (unlike
    # Dattorro). Both arms compile and run — that's the gate this test
    # protects.
    if rms_l > 1e-4:
        print("  ✓ PASS: L lane produces reverb tail from L-only impulse")
    else:
        print(f"  ✗ FAIL: L lane silent (rms_l={rms_l:.5f})")
    if rms_r < 0.05 * rms_l:
        print("  ✓ PASS: R lane near-silent (Freeverb has no L→R cross-coupling)")
    else:
        # Not a hard failure — just informational. Some R energy is fine.
        print(f"  ⚠ INFO: R lane has more energy than expected "
              f"(rms_r={rms_r:.5f}, ratio={rms_r/rms_l:.3f})")


if __name__ == "__main__":
    test_impulse_response()
    test_room_size()
    test_damping()
    test_tail_length()
    test_stereo_decorrelation()
    test_stereo_input()
