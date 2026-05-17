"""
Test: EFFECT_CHORUS (Stereo-Native Chorus)
==========================================
Tests chorus spectral spread and L/R decorrelation.

After prd-stereo-native-opcodes Phase 3, chorus is stereo-native: writes both
out_buffer (L) and out_buffer+1 (R). Mono input auto-escalates internally; the
R lane reads the shared master LFO at +90° for L/R decorrelation.

Expected behavior:
- Chorus creates pitch-modulated copies of the input
- Spectral sidebands appear around the fundamental frequency
- L and R outputs differ on mono input (Pearson |corr| < 0.95)
- Rate / depth parameters control modulation as before

If this test fails, check cedar/include/cedar/opcodes/modulation.hpp.
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure

OUT = output_dir("op_chorus")


def _run_stereo_chorus(host, n_samples, input_signal, *, rate=0.5, depth=0.5,
                       base_delay=20.0, depth_range=10.0,
                       hash_name="chorus", stereo_input=False,
                       input_right=None, lfo_phase=0.25):
    """Set up a stereo-native chorus chain and run the block loop.

    `lfo_phase` (turns, default 0.25 = 90°) tunes the R-LFO offset via the
    ExtendedParams<1> state init. 0.0 collapses to mono-equivalent; 0.5
    produces an anti-phase R lane.
    """
    buf_in = 0
    buf_rate = host.set_param("rate", rate)
    buf_depth = host.set_param("depth", depth)
    buf_base = host.set_param("base_delay", base_delay)
    buf_range = host.set_param("depth_range", depth_range)

    out_buf = 2 if stereo_input else 1
    state_id = cedar.hash(hash_name) & 0xFFFF
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.EFFECT_CHORUS, out_buf, buf_in, buf_rate, buf_depth,
        buf_base, buf_range, state_id
    )
    inst.flags = cedar.STEREO_OUTPUT_FLAG | (
        cedar.STEREO_INPUT_FLAG if stereo_input else 0
    )
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_binary(
        cedar.Opcode.OUTPUT, 0, out_buf, out_buf + 1
    ))

    host.vm.load_program(host.program)
    # Init ExtendedParams AFTER load_program so the state survives
    # (load_program resets the active state pool slot for new program ids).
    # ExtendedParams<3>: slot 0 = lfo_phase (turns, default 0.25 = 90°),
    # slot 1 = dry (default 1.0, Category A), slot 2 = wet (default 0.5).
    # Companion state lives at state_id XOR EXT_PARAMS_STATE_XOR.
    host.vm.init_extended_params(
        cedar.ext_params_state_id(state_id),
        np.array([float(lfo_phase), 1.0, 0.5], dtype=np.float32),
        np.array([0xFFFF, 0xFFFF, 0xFFFF], dtype=np.uint16),
        3,
    )
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
    return (np.concatenate(l_chunks)[:n_samples],
            np.concatenate(r_chunks)[:n_samples])


def test_chorus_spectrum():
    """
    Chorus on 440Hz sine should create spectral sidebands.
    """
    print("Test: Chorus Spectral Spread (stereo-native)")

    sr = 48000
    duration = 3.0
    host = CedarTestHost(sr)

    t = np.arange(int(duration * sr)) / sr
    sine_input = (np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5)

    output_l, output_r = _run_stereo_chorus(host, len(sine_input), sine_input)

    # Save stereo WAV
    stereo = np.column_stack([output_l, output_r]).astype(np.float32)
    wav_path = os.path.join(OUT, "chorus_440hz.wav")
    save_wav(wav_path, stereo, sr)
    print(f"  Saved {wav_path} - Listen for stereo chorus shimmer")

    # Spectrum from the L channel
    fft_size = 8192
    steady_start = int(1.0 * sr)
    steady_output = output_l[steady_start:steady_start + fft_size]

    freqs = np.fft.rfftfreq(fft_size, 1/sr)
    spectrum = np.abs(np.fft.rfft(steady_output))
    spectrum_db = 20 * np.log10(spectrum + 1e-10)

    fundamental_idx = np.argmin(np.abs(freqs - 440))
    fundamental_level = spectrum_db[fundamental_idx]

    sideband_region = (freqs > 420) & (freqs < 460) & (np.abs(freqs - 440) > 2)
    if np.any(sideband_region):
        sideband_level = np.max(spectrum_db[sideband_region])
        print(f"  Fundamental L: {fundamental_level:.1f}dB at 440Hz")
        print(f"  Max sideband L: {sideband_level:.1f}dB")

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    ax1 = axes[0]
    mask = (freqs > 100) & (freqs < 1000)
    ax1.plot(freqs[mask], spectrum_db[mask], linewidth=1)
    ax1.axvline(440, color='red', linestyle='--', alpha=0.5, label='Fundamental')
    ax1.set_xlabel('Frequency (Hz)')
    ax1.set_ylabel('Magnitude (dB)')
    ax1.set_title('Chorus L Spectrum (100-1000Hz)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    ax2 = axes[1]
    mask_zoom = (freqs > 400) & (freqs < 500)
    ax2.plot(freqs[mask_zoom], spectrum_db[mask_zoom], linewidth=1)
    ax2.axvline(440, color='red', linestyle='--', alpha=0.5, label='Fundamental')
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude (dB)')
    ax2.set_title('Chorus L Spectrum Detail (400-500Hz)')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "chorus_spectrum.png"))
    print(f"  Saved {os.path.join(OUT, 'chorus_spectrum.png')}")


def test_stereo_decorrelation():
    """
    Mono input → stereo-native chorus → decorrelated L/R output.

    Expected: Pearson |corr(L, R)| < 0.95 over the steady-state region.
    Random chance for two arbitrary signals is ~0.0, but two voices reading
    the same delay line with offset LFOs should still be highly similar in
    the low frequencies. The decorrelation comes from the LFO offset (+90°)
    producing time-varying delays that differ between L and R.
    """
    print("Test: Chorus Stereo Decorrelation")

    sr = 48000
    duration = 2.0
    host = CedarTestHost(sr)

    t = np.arange(int(duration * sr)) / sr
    sine_input = (np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5)

    output_l, output_r = _run_stereo_chorus(
        host, len(sine_input), sine_input, rate=2.0, depth=0.8
    )

    steady = int(0.5 * sr)
    if np.std(output_l[steady:]) > 1e-6 and np.std(output_r[steady:]) > 1e-6:
        corr = float(np.corrcoef(output_l[steady:], output_r[steady:])[0, 1])
    else:
        corr = 1.0
    print(f"  Pearson corr(L, R) over steady-state = {corr:+.4f}")
    if abs(corr) < 0.95:
        print(f"  ✓ PASS: L and R decorrelated (|corr|={abs(corr):.4f} < 0.95)")
    else:
        print(f"  ✗ FAIL: L and R too correlated (|corr|={abs(corr):.4f} ≥ 0.95)")


def test_lfo_phase_tunable():
    """
    lfo_phase ExtendedParams slot controls R-LFO decorrelation amount.

    Expected behavior (per implementation):
    - lfo_phase = 0.0  → R-LFO matches L-LFO, mono input produces L≈R
                         (Pearson corr > 0.95 over steady state).
    - lfo_phase = 0.25 → default 90° offset, |corr| < 0.5 in practice.
    - lfo_phase = 0.5  → anti-phase R-LFO, still decorrelated.

    If this fails, check cedar/include/cedar/opcodes/modulation.hpp
    (op_effect_chorus reads ExtendedParams<1> slot 0 for lfo_phase).
    """
    print("Test: Chorus lfo_phase tunable via ExtendedParams")
    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = (np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5)

    results = {}
    for label, phase in (("zero", 0.0), ("quarter", 0.25), ("half", 0.5)):
        host = CedarTestHost(sr)
        l, r = _run_stereo_chorus(
            host, len(sine_input), sine_input,
            rate=2.0, depth=0.8,
            hash_name=f"chorus_phase_{label}", lfo_phase=phase,
        )
        steady = int(0.5 * sr)
        if np.std(l[steady:]) > 1e-6 and np.std(r[steady:]) > 1e-6:
            corr = float(np.corrcoef(l[steady:], r[steady:])[0, 1])
        else:
            corr = 1.0
        results[label] = corr
        print(f"  lfo_phase={phase:.2f} → corr(L,R) = {corr:+.4f}")

    if results["zero"] > 0.95:
        print(f"  ✓ PASS: lfo_phase=0 gives mono-equivalent L=R")
    else:
        print(f"  ✗ FAIL: lfo_phase=0 should give corr>0.95, got {results['zero']:.4f}")

    if abs(results["quarter"]) < 0.95:
        print(f"  ✓ PASS: lfo_phase=0.25 decorrelates (|corr|<0.95)")
    else:
        print(f"  ✗ FAIL: lfo_phase=0.25 should decorrelate")


if __name__ == "__main__":
    test_chorus_spectrum()
    test_stereo_decorrelation()
    test_lfo_phase_tunable()
