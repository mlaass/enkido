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
                        input_right=None, lfo_phase=0.25):
    """Set up a stereo-native flanger and run the block loop.

    `lfo_phase` (turns, default 0.25 = 90°) tunes R-LFO offset via the
    ExtendedParams<1> companion state.
    """
    buf_in = 0
    buf_rate = host.set_param("rate", rate)
    buf_depth = host.set_param("depth", depth)
    buf_mind = host.set_param("min_delay", min_delay)
    buf_maxd = host.set_param("max_delay", max_delay)

    out_buf = 2 if stereo_input else 1
    state_id = cedar.hash(hash_name) & 0xFFFF
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.EFFECT_FLANGER, out_buf, buf_in, buf_rate, buf_depth,
        buf_mind, buf_maxd, state_id
    )
    # Pack feedback into high 4 bits of rate field (legacy — flanger
    # feedback has not yet been migrated to ExtendedParams; that's a
    # follow-up per-family PRD).
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
    # ExtendedParams<3>: slot 0 = lfo_phase, slot 1 = dry (1.0, Cat A),
    # slot 2 = wet (0.5, Cat A). Call after load_program so the state
    # survives the program load.
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


def test_lfo_phase_tunable():
    """
    lfo_phase ExtendedParams slot controls R-LFO decorrelation.

    Expected:
    - lfo_phase=0   → R-LFO == L-LFO, corr(L,R) > 0.95 (mono-equivalent)
    - lfo_phase=0.25 → default 90° offset, |corr| < 0.95
    - lfo_phase=0.5 → anti-phase, still decorrelated

    See cedar/include/cedar/opcodes/modulation.hpp op_effect_flanger.
    """
    print("Test: Flanger lfo_phase tunable via ExtendedParams")
    sr = 48000
    duration = 2.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = (np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5)

    results = {}
    for label, phase in (("zero", 0.0), ("quarter", 0.25), ("half", 0.5)):
        host = CedarTestHost(sr)
        l, r = _run_stereo_flanger(
            host, len(sine_input), sine_input,
            rate=1.0, depth=0.7,
            hash_name=f"flanger_phase_{label}", lfo_phase=phase,
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
        print(f"  ✓ PASS: lfo_phase=0.25 decorrelates")
    else:
        print(f"  ✗ FAIL: lfo_phase=0.25 should decorrelate")


if __name__ == "__main__":
    test_flanger_sweep()
    test_stereo_decorrelation()
    test_lfo_phase_tunable()
