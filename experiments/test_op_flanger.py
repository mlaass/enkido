"""
Test: EFFECT_FLANGER (Stereo-Native Flanger)
============================================
Tests flanger sweeping comb filter effect and L/R decorrelation.

After prd-stereo-native-opcodes Phase 3, flanger is stereo-native: writes
both out_buffer (L) and out_buffer+1 (R). Mono input auto-escalates; R lane
reads the master LFO at +90° for L/R decorrelation.

Expected behavior:
- Flanger creates moving comb filter notches in the spectrum
- Spectrogram shows periodic notch sweeping
- L and R decorrelate on mono input (Pearson |corr| < 0.95)

If this test fails, check cedar/include/cedar/opcodes/modulation.hpp.
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_white_noise, save_wav
from visualize import save_figure

OUT = output_dir("op_flanger")


def _run_stereo_flanger(host, n_samples, input_signal, *, rate=0.5, depth=0.8,
                        min_delay=0.1, max_delay=10.0, feedback=0.7,
                        hash_name="flanger", stereo_input=False,
                        input_right=None):
    """Set up a stereo-native flanger and run the block loop."""
    buf_in = 0
    buf_rate = host.set_param("rate", rate)
    buf_depth = host.set_param("depth", depth)
    buf_mind = host.set_param("min_delay", min_delay)
    buf_maxd = host.set_param("max_delay", max_delay)

    out_buf = 2 if stereo_input else 1
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.EFFECT_FLANGER, out_buf, buf_in, buf_rate, buf_depth,
        buf_mind, buf_maxd, cedar.hash(hash_name) & 0xFFFF
    )
    # Pack feedback into high 4 bits of rate field (matches packing used by
    # codegen for the phaser/flanger literal-only feedback parameter).
    fb_idx = max(0, min(15, int(round((feedback + 1.0) * 7.5))))
    inst.rate = (fb_idx << 4) & 0xFF
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
    return (np.concatenate(l_chunks)[:n_samples],
            np.concatenate(r_chunks)[:n_samples])


def test_flanger_sweep():
    """Flanger on white noise produces sweeping comb filter notches."""
    print("Test: Flanger Sweep Pattern (stereo-native)")

    sr = 48000
    duration = 4.0
    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    output_l, output_r = _run_stereo_flanger(host, len(noise), noise)

    stereo = np.column_stack([output_l, output_r]).astype(np.float32)
    wav_path = os.path.join(OUT, "flanger_sweep.wav")
    save_wav(wav_path, stereo, sr)
    print(f"  Saved {wav_path} - Listen for stereo sweeping comb filter")

    fig, axes = plt.subplots(2, 1, figsize=(14, 10))
    ax1 = axes[0]
    ax1.specgram(output_l, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax1.set_ylabel('Frequency (Hz)')
    ax1.set_xlabel('Time (s)')
    ax1.set_title('Flanger L Spectrogram (0.5Hz sweep, 0.7 feedback)')
    ax1.set_ylim(0, 5000)

    ax2 = axes[1]
    ax2.specgram(output_r, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax2.set_ylabel('Frequency (Hz)')
    ax2.set_xlabel('Time (s)')
    ax2.set_title('Flanger R Spectrogram — notches offset by +90° LFO')
    ax2.set_ylim(0, 5000)

    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "flanger_spectrogram.png"))
    print(f"  Saved {os.path.join(OUT, 'flanger_spectrogram.png')}")


def test_stereo_decorrelation():
    """Mono input → stereo flanger → L and R should differ over steady-state."""
    print("Test: Flanger Stereo Decorrelation")

    sr = 48000
    duration = 2.0
    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    output_l, output_r = _run_stereo_flanger(
        host, len(noise), noise, rate=2.0, depth=0.9
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


if __name__ == "__main__":
    test_flanger_sweep()
    test_stereo_decorrelation()
