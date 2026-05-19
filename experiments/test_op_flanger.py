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
                        input_right=None, lfo_phase=0.25, init_ext=True):
    """Set up a stereo-native flanger and run the block loop.

    feedback moved from inst.rate bit-packing to ExtendedParams<5> slot 0
    (prd-extended-params-migration §4.6). `lfo_phase` (turns, default 0.25 =
    90°) tunes the R-LFO offset. When init_ext is False the ExtendedParams
    state is left uninitialized to exercise the opcode's nullptr fallback
    (feedback=-0.99, lfo_phase=0.25).
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
    inst.rate = 0  # rate field no longer carries feedback
    inst.flags = cedar.STEREO_OUTPUT_FLAG | (
        cedar.STEREO_INPUT_FLAG if stereo_input else 0
    )
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_binary(
        cedar.Opcode.OUTPUT, 0, out_buf, out_buf + 1
    ))

    host.vm.load_program(host.program)
    # ExtendedParams<5> (4 used): [feedback, lfo_phase, dry, wet].
    # Call after load_program so the state survives the program load.
    if init_ext:
        host.vm.init_extended_params(
            cedar.ext_params_state_id(state_id),
            np.array([float(feedback), float(lfo_phase), 1.0, 0.5],
                     dtype=np.float32),
            np.array([0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF], dtype=np.uint16),
            4,
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


def test_default_fallback_equivalence():
    """
    Verify the nullptr-fallback (no ExtendedParams init) is identical to an
    explicit-default init (feedback=-0.99, lfo_phase=0.25, dry=1, wet=0.5).

    Pre-migration, flanger feedback came from inst.rate=0 → -0.99. The
    post-migration opcode must reproduce that when ExtendedParams is absent.

    Acceptance: peak sample error < -60 dBFS between the two renders (L+R).
    """
    print("Test: flanger default-fallback equivalence")
    sr = 48000
    noise = gen_white_noise(1.0, sr)

    host_fb = CedarTestHost(sr)
    l_fb, r_fb = _run_stereo_flanger(host_fb, len(noise), noise,
                                     feedback=-0.99, hash_name="fl_fb",
                                     init_ext=False)
    host_def = CedarTestHost(sr)
    l_def, r_def = _run_stereo_flanger(host_def, len(noise), noise,
                                       feedback=-0.99, lfo_phase=0.25,
                                       hash_name="fl_def")

    peak_err = max(float(np.max(np.abs(l_fb - l_def))),
                   float(np.max(np.abs(r_fb - r_def))))
    peak_err_db = 20.0 * np.log10(peak_err + 1e-12)
    print(f"  Peak error fallback vs default-init: {peak_err_db:.1f} dBFS")

    if peak_err_db < -60.0:
        print("  ✓ PASS: nullptr fallback matches explicit defaults")
        return True
    print("  ✗ FAIL: nullptr fallback DIVERGES from explicit defaults")
    return False


def test_feedback_tunable():
    """
    Verify flanger `feedback` is now tunable via ExtendedParams.

    Feedback sharpens and intensifies the comb resonances. A swept-noise
    render with high feedback has a more peaked spectrum (higher
    peak-to-median ratio) than one with near-zero feedback.

    Acceptance: spectral peak-to-median ratio at feedback=0.9 exceeds that
    at feedback=0.0 by ≥ 3 dB.
    """
    print("Test: flanger feedback tunability")
    sr = 48000
    noise = gen_white_noise(3.0, sr)

    def peakiness(sig):
        steady = sig[int(0.5 * sr):]
        spec = np.abs(np.fft.rfft(steady))
        spec_db = 20.0 * np.log10(spec + 1e-9)
        return float(np.max(spec_db) - np.median(spec_db))

    host_lo = CedarTestHost(sr)
    l_lo, r_lo = _run_stereo_flanger(host_lo, len(noise), noise,
                                     rate=0.5, depth=0.8, feedback=0.0,
                                     hash_name="fl_lo")
    host_hi = CedarTestHost(sr)
    l_hi, r_hi = _run_stereo_flanger(host_hi, len(noise), noise,
                                     rate=0.5, depth=0.8, feedback=0.9,
                                     hash_name="fl_hi")

    save_wav(os.path.join(OUT, "feedback_low.wav"),
             np.column_stack([l_lo, r_lo]).astype(np.float32), sr)
    save_wav(os.path.join(OUT, "feedback_high.wav"),
             np.column_stack([l_hi, r_hi]).astype(np.float32), sr)
    print("  Saved feedback_low.wav / feedback_high.wav - listen for resonance")

    peak_lo = peakiness(l_lo)
    peak_hi = peakiness(l_hi)
    diff = peak_hi - peak_lo
    print(f"  Spectral peakiness: feedback=0.0 → {peak_lo:.1f} dB, "
          f"feedback=0.9 → {peak_hi:.1f} dB")
    print(f"  Difference: {diff:.2f} dB")

    if diff >= 3.0:
        print("  ✓ PASS: feedback param is tunable")
        return True
    print("  ✗ FAIL: feedback param has NO measurable effect")
    return False


if __name__ == "__main__":
    test_flanger_sweep()
    test_stereo_decorrelation()
    test_lfo_phase_tunable()
    test_default_fallback_equivalence()
    test_feedback_tunable()
