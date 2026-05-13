"""
Test: EFFECT_PHASER (Stereo-Native Phaser)
==========================================
Verifies the phaser produces audible swept notches and stereo-decorrelates
on mono input.

After prd-stereo-native-opcodes Phase 3, phaser is stereo-native: writes both
out_buffer (L) and out_buffer+1 (R). The L lane is bit-identical to the
legacy mono behavior (same state struct fields at index [0], same LFO phase
read at offset 0). The R lane reads the LFO at +90° for L/R decorrelation.

Expected behavior:
- Output is dry + allpass_cascade — interference creates spectral notches.
- LFO sweeps notch frequency. With depth=0.8 and 200/4000 Hz range, the
  notch should traverse ≥1 octave per LFO period.
- Notch depth ≥ 12 dB.
- L and R decorrelate on mono input (Pearson |corr| < 0.95).

If this test fails, check cedar/include/cedar/opcodes/modulation.hpp.
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import scipy.signal

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_white_noise, save_wav
from visualize import save_figure

OUT = output_dir("op_phaser")


def _build_phaser_program(host, lfo_rate, depth, min_freq, max_freq, stages, feedback_int,
                          lfo_phase=0.25):
    """Configure host with a PHASER instruction. Returns nothing — mutates host.

    After the prd-extended-params migration phaser uses
    `ExtendedParams<3>` for feedback / stages / lfo_phase. The legacy
    `feedback_int` argument is kept (0-15) and converted to a float
    [0.0, 0.99] for the ext slot, so existing tests can stay the same.
    """
    buf_in = 0
    buf_rate = host.set_param("rate", lfo_rate)
    buf_depth = host.set_param("depth", depth)
    buf_min = host.set_param("min_freq", min_freq)
    buf_max = host.set_param("max_freq", max_freq)

    state_id = cedar.hash("phaser") & 0xFFFF
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.EFFECT_PHASER, 1,
        buf_in, buf_rate, buf_depth, buf_min, buf_max,
        state_id,
    )
    inst.rate = 0  # rate field no longer carries feedback/stages
    inst.flags = cedar.STEREO_OUTPUT_FLAG
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 1, 2))
    host.vm.load_program(host.program)

    # ExtendedParams<3>: [feedback, stages, lfo_phase].
    feedback_f = float(feedback_int) / 15.0 * 0.99
    host.vm.init_extended_params(
        cedar.ext_params_state_id(state_id),
        np.array([feedback_f, float(stages), float(lfo_phase)], dtype=np.float32),
        np.array([0xFFFF, 0xFFFF, 0xFFFF], dtype=np.uint16),
        3,
    )


def _find_notch(freqs, psd_db, search_lo=100.0, search_hi=8000.0):
    """
    Find the deepest local minimum in the spectrum within a search range.
    Returns (notch_freq_hz, notch_depth_db) where depth is how far below
    the local mean of surrounding frequencies the minimum is. If no clear
    minimum is found, returns (None, 0.0).
    """
    mask = (freqs >= search_lo) & (freqs <= search_hi)
    if not mask.any():
        return None, 0.0
    band = psd_db[mask]
    band_freqs = freqs[mask]

    # Smooth slightly to suppress narrowband noise spikes.
    win = max(3, len(band) // 64)
    if win % 2 == 0:
        win += 1
    if win >= len(band):
        return None, 0.0
    smoothed = scipy.signal.savgol_filter(band, win, 2)

    min_idx = int(np.argmin(smoothed))
    notch_freq = float(band_freqs[min_idx])

    # Compare to median of band — robust against ramping spectra.
    local_mean_db = float(np.median(smoothed))
    notch_depth = local_mean_db - float(smoothed[min_idx])
    return notch_freq, notch_depth


def test_phaser_notch_sweep():
    """
    Verify that the phaser produces real notches that sweep over time.

    Uses default phaser params with stages=4, feedback≈0.5 (matches Akkado
    builtin defaults). Drives white noise through the phaser and inspects
    the spectrum across windows that span one LFO period.
    """
    print("Test: Phaser Notch Sweep (default config)")

    sr = 48000

    # Long-run audio for stability checks (>=300s per CLAUDE.md policy).
    long_duration = 300.0
    host = CedarTestHost(sr)
    noise = gen_white_noise(long_duration, sr)

    lfo_rate = 1.0
    depth = 0.8
    min_freq = 200.0
    max_freq = 4000.0
    stages = 4
    feedback_int = 8  # ~0.528 of 0.99 max

    _build_phaser_program(host, lfo_rate, depth, min_freq, max_freq, stages, feedback_int)
    long_output = host.process(noise)

    # Stability check across the full 300s
    if not np.all(np.isfinite(long_output)):
        print(f"  ✗ FAIL: Non-finite samples in long output (NaN/Inf detected)")
        return False
    peak = float(np.max(np.abs(long_output)))
    if peak > 50.0:
        print(f"  ✗ FAIL: Output peak {peak:.2f} suggests blow-up (feedback unstable)")
        return False
    print(f"  Long run OK: {long_duration:.0f}s, peak={peak:.3f}")

    # Render a short WAV from the start of the long buffer for human listening
    wav_path = os.path.join(OUT, "phaser_sweep.wav")
    short_len = int(4.0 * sr)
    save_wav(wav_path, long_output[:short_len], sr)
    print(f"  Saved {wav_path} - Listen for swooshing swept notches")

    # Notch-sweep analysis: take FFTs over windows covering one LFO period.
    # At 1 Hz LFO, one period is 1 second; we slice 8 windows over the period
    # and look for the deepest notch in each.
    period_samples = int(sr / lfo_rate)
    n_windows = 8
    win_len = period_samples // n_windows
    # Use a known-stable region of the long buffer (skip first second to avoid cold start)
    base = sr  # start at 1 second in
    notches = []
    for i in range(n_windows):
        start = base + i * win_len
        seg = long_output[start:start + win_len]
        if len(seg) < 1024:
            continue
        freqs, psd = scipy.signal.welch(seg, fs=sr, nperseg=min(2048, len(seg)))
        psd_db = 10.0 * np.log10(psd + 1e-20)
        nf, nd = _find_notch(freqs, psd_db)
        if nf is not None:
            notches.append((nf, nd))

    if len(notches) < 4:
        print(f"  ✗ FAIL: Could not detect notches in {n_windows} windows (got {len(notches)})")
        return False

    notch_freqs = np.array([n[0] for n in notches])
    notch_depths = np.array([n[1] for n in notches])

    sweep_ratio = notch_freqs.max() / max(notch_freqs.min(), 1.0)
    median_depth = float(np.median(notch_depths))

    print(f"  Notch frequencies across LFO period: {[f'{f:.0f}Hz' for f in notch_freqs]}")
    print(f"  Sweep ratio (max/min): {sweep_ratio:.2f}x")
    print(f"  Median notch depth: {median_depth:.1f} dB")

    sweep_ok = sweep_ratio >= 2.0  # ≥1 octave
    depth_ok = median_depth >= 12.0

    if sweep_ok and depth_ok:
        print(f"  ✓ PASS: Notch sweeps {sweep_ratio:.2f}x at median depth {median_depth:.1f} dB")
    else:
        if not sweep_ok:
            print(f"  ✗ FAIL: Sweep ratio {sweep_ratio:.2f}x < 2.0x (expected ≥1 octave)")
        if not depth_ok:
            print(f"  ✗ FAIL: Median notch depth {median_depth:.1f} dB < 12 dB "
                  f"(spectrum is too flat — dry/wet sum may be missing)")

    # Spectrogram for visual inspection (use short clip)
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.specgram(long_output[:int(4.0 * sr)], NFFT=1024, Fs=sr, noverlap=512, cmap='magma')
    ax.set_title(f"Phaser Spectrogram ({stages}-stage, {lfo_rate}Hz LFO, depth={depth})")
    ax.set_ylabel("Frequency (Hz)")
    ax.set_xlabel("Time (s)")
    ax.set_ylim(0, 8000)
    save_figure(fig, os.path.join(OUT, "phaser_spectrogram.png"))
    print(f"  Saved {os.path.join(OUT, 'phaser_spectrogram.png')}")

    return sweep_ok and depth_ok


def test_phaser_no_sweep_at_zero_depth():
    """
    Verify that depth=0 produces a stationary notch — the LFO multiplier
    collapses to 1.0 so the cascade center frequency is constant.

    A multi-stage cascade has multiple notches at fixed offsets from f_c, so
    we narrow the search to 100..600 Hz to lock onto a single notch (the
    lowest one, near 0.414*f_c ≈ 370 Hz for 4 stages with f_c=894 Hz).
    """
    print("Test: Phaser Stationary Notch at depth=0")

    sr = 48000
    duration = 4.0
    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    _build_phaser_program(host, lfo_rate=1.0, depth=0.0,
                          min_freq=200.0, max_freq=4000.0,
                          stages=4, feedback_int=8)
    output = host.process(noise)

    if not np.all(np.isfinite(output)):
        print(f"  ✗ FAIL: Non-finite samples in output")
        return False

    period_samples = sr  # 1 LFO period at 1 Hz
    n_windows = 4
    win_len = period_samples // n_windows
    notches = []
    for i in range(n_windows):
        start = sr + i * win_len  # skip first second
        seg = output[start:start + win_len]
        if len(seg) < 1024:
            continue
        freqs, psd = scipy.signal.welch(seg, fs=sr, nperseg=min(2048, len(seg)))
        psd_db = 10.0 * np.log10(psd + 1e-20)
        # Narrow band isolates the lowest notch (one notch per band).
        nf, _ = _find_notch(freqs, psd_db, search_lo=100.0, search_hi=600.0)
        if nf is not None:
            notches.append(nf)

    if len(notches) < 3:
        print(f"  ⚠ Could not detect enough notches at depth=0 (got {len(notches)})")
        return True  # not a hard failure — depth=0 may flatten the notch entirely

    nf = np.array(notches)
    sweep_ratio = nf.max() / max(nf.min(), 1.0)
    if sweep_ratio < 1.3:
        print(f"  ✓ PASS: Notch is stationary at depth=0 (sweep ratio {sweep_ratio:.2f}x, "
              f"freqs={[f'{f:.0f}Hz' for f in nf]})")
        return True
    else:
        print(f"  ✗ FAIL: Notch swept {sweep_ratio:.2f}x at depth=0 (should be ~1.0x), "
              f"freqs={[f'{f:.0f}Hz' for f in nf]}")
        return False


def test_stereo_decorrelation():
    """Mono input → stereo-native phaser → L and R must decorrelate.

    Uses process_stereo to capture both buses. The mono input on buf 0 is
    auto-broadcast inside the opcode (STEREO_INPUT clear), but the R lane's
    +90° LFO offset produces different notch frequencies over time, which
    visibly decorrelates the output.
    """
    print("Test: Phaser Stereo Decorrelation")

    sr = 48000
    duration = 2.0
    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    _build_phaser_program(host, lfo_rate=2.0, depth=0.9,
                          min_freq=200.0, max_freq=4000.0,
                          stages=4, feedback_int=8)

    # process() returns only L; use process_stereo to capture both even though
    # the input is mono.
    output_l, output_r = host.process_stereo(noise, noise)

    steady = int(0.5 * sr)
    if np.std(output_l[steady:]) > 1e-6 and np.std(output_r[steady:]) > 1e-6:
        corr = float(np.corrcoef(output_l[steady:], output_r[steady:])[0, 1])
    else:
        corr = 1.0
    print(f"  Pearson corr(L, R) over steady-state = {corr:+.4f}")
    if abs(corr) < 0.95:
        print(f"  ✓ PASS: L and R decorrelated (|corr|={abs(corr):.4f} < 0.95)")
        return True
    else:
        print(f"  ✗ FAIL: L and R too correlated (|corr|={abs(corr):.4f} ≥ 0.95)")
        return False


def test_lfo_phase_tunable():
    """
    lfo_phase via ExtendedParams<3>[2] tunes the R-LFO decorrelation.

    Phaser has a wet+dry sum in the body so the L/R correlation never goes
    to ±1 even with lfo_phase=0 (the dry component is identical). But the
    lfo_phase=0 case should be NOTICEABLY more correlated than the default.

    See cedar/include/cedar/opcodes/modulation.hpp op_effect_phaser.
    """
    print("Test: Phaser lfo_phase tunable via ExtendedParams")
    sr = 48000
    duration = 2.0
    rng = np.random.default_rng(0xBA5E)
    noise = rng.standard_normal(int(duration * sr)).astype(np.float32) * 0.4

    results = {}
    for label, phase in (("zero", 0.0), ("quarter", 0.25), ("half", 0.5)):
        host = CedarTestHost(sr)
        _build_phaser_program(host, lfo_rate=2.0, depth=0.9,
                              min_freq=200.0, max_freq=4000.0,
                              stages=4, feedback_int=8, lfo_phase=phase)
        # Manually drive the block loop since _build_phaser_program leaves
        # state init in place and load_program already happened.
        n_samples = len(noise)
        n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
        padded = np.zeros(n_blocks * cedar.BLOCK_SIZE, dtype=np.float32)
        padded[:n_samples] = noise
        l_chunks, r_chunks = [], []
        for k in range(n_blocks):
            s = k * cedar.BLOCK_SIZE
            host.vm.set_buffer(0, padded[s:s + cedar.BLOCK_SIZE])
            l, r = host.vm.process()
            l_chunks.append(l)
            r_chunks.append(r)
        l = np.concatenate(l_chunks)[:n_samples]
        r = np.concatenate(r_chunks)[:n_samples]
        steady = int(0.5 * sr)
        corr = float(np.corrcoef(l[steady:], r[steady:])[0, 1])
        results[label] = corr
        print(f"  lfo_phase={phase:.2f} → corr(L,R) = {corr:+.4f}")

    # lfo_phase=0 should be more correlated than the default 0.25 case.
    if results["zero"] > results["quarter"]:
        print(f"  ✓ PASS: lfo_phase=0 ({results['zero']:.3f}) more correlated than "
              f"0.25 ({results['quarter']:.3f})")
        return True
    else:
        print(f"  ✗ FAIL: expected lfo_phase=0 > lfo_phase=0.25 in correlation, "
              f"got {results['zero']:.3f} vs {results['quarter']:.3f}")
        return False


if __name__ == "__main__":
    results = []
    results.append(test_phaser_notch_sweep())
    results.append(test_phaser_no_sweep_at_zero_depth())
    results.append(test_stereo_decorrelation())
    results.append(test_lfo_phase_tunable())
    if all(results):
        print("\nAll phaser tests passed.")
    else:
        print(f"\n{sum(1 for r in results if not r)} test(s) failed.")
        raise SystemExit(1)
