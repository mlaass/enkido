"""
Test: REVERB_FDN (4x4 Hadamard Feedback Delay Network)
=======================================================
Tests FDN reverb impulse response, decay, damping, AND the Phase 2
stereo-native conversion (prd-stereo-native-opcodes).

Expected behavior:
- Smooth, dense reverb tail (Hadamard matrix ensures good mixing)
- Decay parameter controls RT60
- Damping causes HF rolloff in tail
- 4 delay lines with Hadamard mixing (single shared state pool)
- Output is STEREO: writes out_buffer (L) and out_buffer+1 (R) as diagonal
  Hadamard taps (delayed[0] → L, delayed[1] → R, each scaled by 0.5)
- L/R decorrelated by the prime-ratio spacing between the two diagonal taps
  (1931 vs 2473 samples)
- Stereo input is folded by averaging (FDN has no per-channel topology)

If this test fails, check the implementation in cedar/include/cedar/opcodes/reverbs.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_impulse, gen_noise_pulse, save_wav
from visualize import save_figure

OUT = output_dir("op_fdn")


def _run_stereo_fdn(host, n_samples, input_signal, *, decay=0.85, damp=0.3,
                    hash_name="fdn", stereo_input=False, input_right=None):
    """
    Set up a stereo-native FDN instruction and run the block loop,
    returning (output_l, output_r).
    """
    buf_in = 0
    buf_decay = host.set_param("decay", decay)
    buf_damp = host.set_param("damp", damp)

    out_buf = 2 if stereo_input else 1
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.REVERB_FDN, out_buf, buf_in, buf_decay, buf_damp,
        cedar.hash(hash_name) & 0xFFFF
    )
    inst.rate = 128  # Room size modifier 1.0x
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
    """
    Estimate RT60 via Schroeder backward integration.

    Why backward integration instead of the simple "smooth envelope, find first
    crossing of -60dB" approach used in test_op_freeverb.py: FDN's stereo
    output emits a single Hadamard tap per channel (delayed[0] to L,
    delayed[1] to R) rather than a sum of all 4 taps. Each tap has bursty
    arrival behaviour with inter-burst gaps where the envelope dips to noise
    floor, and a 10ms smoothing window can't bridge those gaps. Schroeder's
    backward integration cumulates the squared response from the end of the
    signal back to t, producing a strictly monotone decay curve that is
    immune to local dips, so the -60dB crossing is well-defined for a
    single-tap output.

    Reference: M. R. Schroeder, "New Method of Measuring Reverberation
    Time," J. Acoust. Soc. Am. 37, 409 (1965).
    """
    sq = signal.astype(np.float64) ** 2
    if np.sum(sq) < 1e-20:
        return 0.0
    # Schroeder integral: ∫_t^∞ h(τ)² dτ — implemented as a reversed cumulative
    # sum of the squared signal.
    schroeder = np.cumsum(sq[::-1])[::-1]
    # Normalise and convert to dB (10*log10 of energy)
    schroeder_db = 10.0 * np.log10(schroeder / schroeder[0] + 1e-20)
    # First sample where the decay curve drops past threshold_db
    below = np.where(schroeder_db < threshold_db)[0]
    if len(below) == 0:
        return float(len(signal)) / sr
    return float(below[0]) / sr


def _mono_metric(output_l, output_r):
    """
    Per-sample maximum-magnitude fold for RT60 metrics.

    FDN's stereo output emits two diagonal Hadamard taps (delayed[0] and
    delayed[1]); these come from a single shared state pool whose Hadamard
    re-injection makes them partially anti-phase. A naive (L+R)/2 average
    therefore has destructive cancellation that doesn't correspond to what
    is actually audible. Using per-sample max(|L|,|R|) tracks the dominant
    channel's envelope and matches the pre-PRD single-tap behaviour for
    RT60 estimation. This is metric-only — the saved WAVs are still true
    stereo for human listening.
    """
    return np.maximum(np.abs(output_l), np.abs(output_r))


def test_impulse_response():
    """
    Test FDN impulse response produces smooth, dense stereo tail.

    Expected:
    - Dense reverb on both channels (no sparse echoes)
    - Smooth exponential decay
    """
    print("Test: REVERB_FDN Impulse Response (stereo-native)")

    sr = 48000
    duration = 3.0
    host = CedarTestHost(sr)
    impulse = gen_impulse(duration, sr)

    output_l, output_r = _run_stereo_fdn(
        host, len(impulse), impulse, decay=0.85, damp=0.3, hash_name="fdn"
    )
    output = _mono_metric(output_l, output_r)

    rt60 = estimate_rt60(output, sr)
    peak_amp = float(max(np.max(np.abs(output_l)), np.max(np.abs(output_r))))

    print(f"  RT60 estimate (mono-fold): {rt60:.2f}s")
    print(f"  Peak amplitude: {peak_amp:.4f}")

    if rt60 > 0.1:
        print("  ✓ PASS: Reverb tail present")
    else:
        print("  ✗ FAIL: No significant reverb tail")

    wav_path = os.path.join(OUT, "fdn_impulse.wav")
    save_wav(wav_path, np.stack([output_l, output_r], axis=1), sr)
    print(f"  Saved {wav_path} - Listen for smooth, dense stereo reverb tail")

    time = np.arange(len(output_l)) / sr
    env_l_db = 20 * np.log10(np.abs(output_l) + 1e-10)
    env_r_db = 20 * np.log10(np.abs(output_r) + 1e-10)

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    axes[0].plot(time, output_l, linewidth=0.3, color='purple', label='L')
    axes[0].plot(time, output_r, linewidth=0.3, color='orange', alpha=0.6, label='R')
    axes[0].set_title("FDN Impulse Response (decay=0.85, damp=0.3)")
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
    save_figure(fig, os.path.join(OUT, "fdn_ir.png"))
    print(f"  Saved {os.path.join(OUT, 'fdn_ir.png')}")


def test_decay_parameter():
    """
    Test decay parameter: longer decay → longer RT60.
    Expected: RT60 increases monotonically with decay
    """
    print("Test: REVERB_FDN Decay Parameter")

    sr = 48000
    duration = 5.0
    impulse = gen_impulse(duration, sr)

    decay_values = [0.3, 0.6, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("FDN Decay Parameter Comparison (mono-fold of stereo output)")

    for decay, ax in zip(decay_values, axes.flat):
        host = CedarTestHost(sr)
        output_l, output_r = _run_stereo_fdn(
            host, len(impulse), impulse, decay=decay, damp=0.3,
            hash_name="fdn_dec"
        )
        output = _mono_metric(output_l, output_r)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        ax.plot(time, env_db, linewidth=0.5)
        ax.set_title(f'Decay={decay} (RT60≈{rt60:.2f}s)')
        ax.set_ylim(-100, 0)
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('dB')
        ax.grid(True, alpha=0.3)

        print(f"  Decay {decay}: RT60 ≈ {rt60:.2f}s")

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "fdn_decay.png"))
    print(f"  Saved {os.path.join(OUT, 'fdn_decay.png')}")

    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with decay")
    else:
        print("  ✗ FAIL: RT60 does not increase monotonically with decay")


def test_damping():
    """
    Test damping: HF rolloff in reverb tail.
    Expected: Higher damping → lower spectral centroid in late tail
    """
    print("Test: REVERB_FDN Damping")

    sr = 48000
    duration = 3.0
    impulse = gen_impulse(duration, sr)

    damp_values = [0.0, 0.3, 0.6, 0.9]
    centroids = []

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("FDN Damping Effect (mono-fold)")

    for damp, ax in zip(damp_values, axes.flat):
        host = CedarTestHost(sr)
        output_l, output_r = _run_stereo_fdn(
            host, len(impulse), impulse, decay=0.85, damp=damp,
            hash_name="fdn_d"
        )
        output = _mono_metric(output_l, output_r)

        # Late tail spectrum
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
    save_figure(fig, os.path.join(OUT, "fdn_damping.png"))
    print(f"  Saved {os.path.join(OUT, 'fdn_damping.png')}")

    is_decreasing = all(centroids[i] >= centroids[i + 1] for i in range(len(centroids) - 1))
    if is_decreasing:
        print("  ✓ PASS: Spectral centroid decreases with damping")
    else:
        print("  ⚠ WARN: Spectral centroid does not decrease monotonically with damping")


def test_tail_length():
    """
    Test reverb tail length with short noise pulse input at different decay values.

    Expected:
    - Noise pulse excites all delay line modes for realistic RT60 measurement
    - RT60 increases monotonically with decay
    - Each setting produces audible stereo reverb tail in WAV output
    """
    print("Test: REVERB_FDN Tail Length (noise pulse)")

    sr = 48000
    duration = 5.0

    decay_values = [0.3, 0.6, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(len(decay_values), 1, figsize=(12, 3 * len(decay_values)))
    fig.suptitle("FDN Tail Length — Noise Pulse Excitation (mono-fold of stereo)")

    for idx, decay in enumerate(decay_values):
        noise_pulse = gen_noise_pulse(duration, sr, pulse_ms=10)

        host = CedarTestHost(sr)
        output_l, output_r = _run_stereo_fdn(
            host, len(noise_pulse), noise_pulse, decay=decay, damp=0.3,
            hash_name="fdn_tail"
        )
        output = _mono_metric(output_l, output_r)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        wav_path = os.path.join(OUT, f"fdn_tail_decay{decay}.wav")
        save_wav(wav_path, np.stack([output_l, output_r], axis=1), sr)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        axes[idx].plot(time, env_db, linewidth=0.5)
        axes[idx].axhline(-60, color='red', linestyle='--', alpha=0.5)
        axes[idx].set_title(f'Decay={decay} (RT60≈{rt60:.2f}s)')
        axes[idx].set_ylim(-100, 0)
        axes[idx].set_ylabel('dB')
        axes[idx].grid(True, alpha=0.3)

        print(f"  Decay {decay}: RT60 ≈ {rt60:.2f}s")
        print(f"  Saved {wav_path} - Listen for stereo reverb tail length")

    axes[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "fdn_tail_length.png"))
    print(f"  Saved {os.path.join(OUT, 'fdn_tail_length.png')}")

    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with decay (noise pulse)")
    else:
        print(f"  ✗ FAIL: RT60 not monotonic: {rt60s}")


def test_stereo_decorrelation():
    """
    Mono input → stereo-native FDN → decorrelated L/R output.

    Decorrelation comes from emitting two different diagonal Hadamard taps
    (delayed[0] vs delayed[1]) whose delay lines have prime-ratio spacing
    (1931 vs 2473 samples). Pearson correlation between L and R must be
    < 0.999.
    """
    print("Test: REVERB_FDN Stereo Decorrelation")

    sr = 48000
    duration = 2.0
    host = CedarTestHost(sr)
    impulse = gen_impulse(duration, sr)

    output_l, output_r = _run_stereo_fdn(
        host, len(impulse), impulse, decay=0.85, damp=0.3,
        hash_name="fdn_decorr"
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

    wav_path = os.path.join(OUT, "fdn_decorrelation.wav")
    save_wav(wav_path, np.stack([output_l, output_r], axis=1), sr)
    print(f"  Saved {wav_path} - Listen for stereo width")

    if corr < 0.999:
        print("  ✓ PASS: L and R are non-trivially decorrelated")
    else:
        print(f"  ✗ FAIL: L and R are virtually identical (corr={corr:.6f})")


def test_stereo_input_handling():
    """
    Stereo input → stereo-native FDN runs without crashing and produces a
    sensible output. FDN folds stereo input by averaging (it has a single
    shared state pool, not per-channel lanes), so we only assert that the
    program compiles + runs and that the stereo flag path produces output
    similar in level to the mono-only equivalent.
    """
    print("Test: REVERB_FDN Stereo Input")

    sr = 48000
    duration = 2.0
    n_samples = int(duration * sr)

    # Mono baseline
    impulse_mono = np.zeros(n_samples, dtype=np.float32)
    impulse_mono[0] = 1.0
    host_mono = CedarTestHost(sr)
    mono_l, mono_r = _run_stereo_fdn(
        host_mono, n_samples, impulse_mono, decay=0.85, damp=0.3,
        hash_name="fdn_stin_m"
    )

    # Stereo input: L = impulse, R = impulse (same as mono after avg).
    # Should produce essentially identical output.
    host_st = CedarTestHost(sr)
    same_l, same_r = _run_stereo_fdn(
        host_st, n_samples, impulse_mono, decay=0.85, damp=0.3,
        hash_name="fdn_stin_s", stereo_input=True, input_right=impulse_mono
    )

    rms_mono = float(np.sqrt(np.mean(mono_l ** 2 + mono_r ** 2)))
    rms_same = float(np.sqrt(np.mean(same_l ** 2 + same_r ** 2)))

    wav_mono = os.path.join(OUT, "fdn_stereo_input_mono.wav")
    save_wav(wav_mono, np.stack([mono_l, mono_r], axis=1), sr)
    wav_same = os.path.join(OUT, "fdn_stereo_input_dup.wav")
    save_wav(wav_same, np.stack([same_l, same_r], axis=1), sr)
    print(f"  rms_mono_in={rms_mono:.5f}, rms_stereo_in_dup={rms_same:.5f}")

    # With identical L=R input, the average fold is identical to the mono
    # signal, so outputs should match very closely.
    if rms_mono > 1e-4 and rms_same > 1e-4:
        print("  ✓ PASS: stereo-input path produces non-trivial output")
    else:
        print(f"  ✗ FAIL: stereo-input path silent (rms_mono={rms_mono}, rms_same={rms_same})")

    # Compare outputs for identical-input cross-check (allow some numerical drift).
    if rms_mono > 1e-4:
        delta = abs(rms_same - rms_mono) / rms_mono
        if delta < 0.05:
            print(f"  ✓ PASS: stereo(dup-mono) matches mono within 5% (delta={delta:.4f})")
        else:
            print(f"  ⚠ INFO: stereo-fold differs by {delta:.4f} from mono baseline")


if __name__ == "__main__":
    test_impulse_response()
    test_decay_parameter()
    test_damping()
    test_tail_length()
    test_stereo_decorrelation()
    test_stereo_input_handling()
