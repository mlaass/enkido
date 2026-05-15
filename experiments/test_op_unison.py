"""
unison Stdlib Function Quality Tests (Cedar Engine)
====================================================
Smoke tests for the userspace `unison(...)` stdlib function. Drives real
akkado source through `nkido-cli render` and verifies:

1. A 5-voice unison around 440 Hz produces exactly 5 spectral peaks within
   the documented detune window (±0.3 semitones).
2. The `voices == 1` special-case branch yields a single centered, undetuned
   voice (one peak at 440 Hz, balanced L/R).
3. Voice-count sweep {2, 4, 8}: each produces the corresponding peak count
   and saves a WAV for ear evaluation.
4. poly + unison composition rendered for 300 s — no NaN/Inf, no clipping,
   no RMS drop-outs (catches voice-stealing / cycle-boundary regressions).

WAVs land in `output/op_unison/`. If a spectral assertion fails, **do not
loosen the threshold** — investigate `akkado/include/akkado/stdlib.hpp`'s
`unison` formula or the poly+unison composition path.
"""

import os
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido-cli" / "nkido-cli"

OUT_DIR = THIS_DIR / "output" / "op_unison"
OUT_DIR.mkdir(parents=True, exist_ok=True)


# -----------------------------------------------------------------------------
# Render + WAV helpers
# -----------------------------------------------------------------------------

def render(source: str, wav_path: Path, seconds: float, bpm: float = 120.0) -> None:
    """Run `nkido-cli render --source ...` and write a WAV. Raise on failure."""
    if not NKIDO_CLI.exists():
        raise FileNotFoundError(
            f"nkido-cli not found at {NKIDO_CLI}. "
            f"Build with: cmake --build {REPO_ROOT}/build --target nkido-cli"
        )
    cmd = [
        str(NKIDO_CLI), "render",
        "--source", source,
        "-o", str(wav_path),
        "--seconds", str(seconds),
        "--bpm", str(bpm),
    ]
    # Long-render timeout: 300 s of audio renders much faster than real time
    # but we still allow generous headroom.
    timeout = max(60.0, seconds * 1.5)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0 or not wav_path.exists():
        raise RuntimeError(
            f"nkido-cli render failed (exit {result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


def load_wav(path: Path) -> tuple[np.ndarray, int]:
    """Load a 16-bit stereo WAV → (samples, sr). samples is shape (frames, 2)."""
    with wave.open(str(path), "rb") as f:
        frames = f.getnframes()
        sr = f.getframerate()
        channels = f.getnchannels()
        sampwidth = f.getsampwidth()
        raw = f.readframes(frames)
    assert sampwidth == 2, f"Expected 16-bit WAV, got {sampwidth*8}-bit"
    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if channels == 2:
        samples = samples.reshape(-1, 2)
    return samples, sr


# -----------------------------------------------------------------------------
# Peak detection
# -----------------------------------------------------------------------------

def find_peaks_in_band(audio: np.ndarray, sr: int,
                       f_lo: float, f_hi: float,
                       rel_threshold: float = 0.30,
                       min_separation_hz: float = 1.5) -> list[float]:
    """
    Find local-maximum FFT bins inside [f_lo, f_hi] that exceed `rel_threshold`
    of the band's peak magnitude. Returns the peak frequencies (Hz), sorted.

    The band should bracket the expected detune spread (±detune semitones
    around the fundamental); too wide and edge leakage shows up as spurious
    extra peaks.

    Adapted from test_chord_pipeline.py::count_distinct_pitch_classes. Used to
    verify unison's voice count matches the expected number of spread peaks
    around the fundamental.
    """
    # Skip the first 50 ms (attack transient before steady-state) for a clean
    # spectrum.
    skip = int(0.05 * sr)
    if skip < len(audio):
        audio = audio[skip:]

    n = len(audio)
    win = audio * np.hanning(n)
    fft = np.fft.rfft(win)
    mag = np.abs(fft) / (n / 2 + 1)
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)

    band_mask = (freqs >= f_lo) & (freqs <= f_hi)
    if not band_mask.any():
        return []
    band_max = float(mag[band_mask].max())
    if band_max == 0.0:
        return []
    threshold = rel_threshold * band_max

    peaks: list[tuple[float, float]] = []  # (freq, mag)
    band_idx = np.where(band_mask)[0]
    for i in band_idx:
        if i == 0 or i == len(mag) - 1:
            continue
        if mag[i] > threshold and mag[i] > mag[i - 1] and mag[i] > mag[i + 1]:
            peaks.append((float(freqs[i]), float(mag[i])))

    # Enforce minimum frequency separation by greedily picking the strongest
    # peak first and dropping any nearer neighbours.
    peaks.sort(key=lambda p: -p[1])
    kept: list[tuple[float, float]] = []
    for f, m in peaks:
        if all(abs(f - kf) >= min_separation_hz for kf, _ in kept):
            kept.append((f, m))
    return sorted(f for f, _ in kept)


# =============================================================================
# Test 1: 5-voice unison spectrum
# =============================================================================

def test_unison_spectrum_5voice():
    """
    Test the canonical PRD example: 5-voice unison around 440 Hz with
    detune=0.3 semitones produces 5 distinct spectral peaks within the
    documented spread.

    Expected per stdlib.hpp::unison and prd-unison.md §5.3:
        linspace(-1, 1, 5)            = [-1, -0.5, 0, 0.5, 1]
        per-voice semitone offsets    = u * 0.3 ∈ [-0.3, -0.15, 0, 0.15, 0.3]
        per-voice frequencies (Hz)    = 440 * 2^(s/12)
                                      ≈ [432.4, 436.2, 440.0, 443.8, 447.7]
    With a 2s render at 48 kHz, FFT bin spacing is 0.5 Hz — comfortably
    resolves the ~3.8 Hz peak spacing.
    """
    print("\nTest 1: 5-voice unison spectrum (PRD canonical case)")
    print("=" * 60)

    source = (
        "fn voice(f, g, v, e) -> saw(f)\n"
        "unison(440, 1, 1, voice, voices: 5, detune: 0.3) |> out(%)\n"
    )
    wav = OUT_DIR / "unison_5voice.wav"
    render(source, wav, seconds=2.0)

    samples, sr = load_wav(wav)
    mono = samples.mean(axis=1) if samples.ndim == 2 else samples
    # Search band: 440 Hz ± detune semitones ≈ 432.4..447.7 Hz, plus a small
    # margin for FFT bin tolerance. Wider bands pick up edge leakage as
    # spurious peaks (especially at higher voice counts).
    peaks = find_peaks_in_band(mono, sr, 431.0, 449.0)

    expected = [432.4, 436.2, 440.0, 443.8, 447.7]
    print(f"  Expected ~5 peaks in 431–449 Hz at {expected}")
    print(f"  Found {len(peaks)} peaks at {[round(p, 2) for p in peaks]}")
    print(f"  Saved {wav} — Listen for fat detuned saw, voices across stereo")

    assert len(peaks) == 5, (
        f"Expected 5 unison peaks in 420–460 Hz, found {len(peaks)} at "
        f"{[round(p, 2) for p in peaks]}"
    )
    # Each found peak should be near an expected position (±1.5 Hz tolerance
    # accounts for ½-bin error + windowing leakage).
    for p in peaks:
        nearest = min(expected, key=lambda e: abs(e - p))
        assert abs(p - nearest) <= 1.5, (
            f"Peak {p:.2f} Hz is >1.5 Hz from nearest expected {nearest:.2f}"
        )
    print("  ✓ PASS: 5 peaks at expected detune positions")


# =============================================================================
# Test 2: voices=1 special-case branch
# =============================================================================

def test_unison_voices_1_special_case():
    """
    Test the `if voices == 1` branch in stdlib.hpp::unison: a single centered,
    undetuned voice with ext = {idx:0, count:1, detune_st:0, pan:0, phase:0}.

    Expected:
    - Exactly 1 spectral peak in 420–460 Hz at 440 Hz.
    - L and R RMS within 5% of each other (panned to center).
    """
    print("\nTest 2: voices=1 special-case branch")
    print("=" * 60)

    source = (
        "fn voice(f, g, v, e) -> saw(f)\n"
        "unison(440, 1, 1, voice, voices: 1) |> out(%)\n"
    )
    wav = OUT_DIR / "unison_voices1.wav"
    render(source, wav, seconds=2.0)

    samples, sr = load_wav(wav)
    assert samples.ndim == 2 and samples.shape[1] == 2, "Expected stereo WAV"
    left, right = samples[:, 0], samples[:, 1]
    mono = (left + right) * 0.5
    peaks = find_peaks_in_band(mono, sr, 431.0, 449.0)
    print(f"  Found {len(peaks)} peaks at {[round(p, 2) for p in peaks]}")

    assert len(peaks) == 1, (
        f"voices=1 special case expects exactly 1 peak; found {len(peaks)}"
    )
    assert abs(peaks[0] - 440.0) <= 1.5, (
        f"Peak {peaks[0]:.2f} Hz is not at 440 Hz — special case may not be "
        f"taking the centered branch"
    )

    # L/R balance check: pan=0 means equal-power center → L_rms ≈ R_rms.
    # Skip the first 50 ms attack to compare steady state.
    skip = int(0.05 * sr)
    l_rms = float(np.sqrt(np.mean(left[skip:] ** 2)))
    r_rms = float(np.sqrt(np.mean(right[skip:] ** 2)))
    assert l_rms > 1e-4 and r_rms > 1e-4, (
        f"Expected audible output; got L_rms={l_rms}, R_rms={r_rms}"
    )
    balance = abs(l_rms - r_rms) / max(l_rms, r_rms)
    print(f"  L_rms={l_rms:.4f} R_rms={r_rms:.4f} balance_err={balance:.3f}")
    assert balance <= 0.05, (
        f"L/R imbalance {balance:.3f} > 5% — voice not centered "
        f"(special-case branch may have drifted)"
    )
    print(f"  Saved {wav} — Listen for single centered saw at 440 Hz")
    print("  ✓ PASS: 1 peak at 440 Hz, L/R balanced within 5%")


# =============================================================================
# Test 3: voices sweep {2, 4, 8}
# =============================================================================

def test_unison_voices_sweep():
    """
    Render unison with voices in {2, 4, 8}, save WAVs, and verify each produces
    exactly N peaks in the detune window. This is the human-ear-friendly part
    of the test — open these in an audio editor and listen to the spread grow.
    """
    print("\nTest 3: voices sweep {2, 4, 8}")
    print("=" * 60)

    for n in [2, 4, 8]:
        source = (
            "fn voice(f, g, v, e) -> saw(f)\n"
            f"unison(440, 1, 1, voice, voices: {n}, detune: 0.3) |> out(%)\n"
        )
        wav = OUT_DIR / f"unison_{n}voice.wav"
        render(source, wav, seconds=2.0)
        samples, sr = load_wav(wav)
        mono = samples.mean(axis=1) if samples.ndim == 2 else samples
        peaks = find_peaks_in_band(mono, sr, 431.0, 449.0)
        print(f"  voices={n}: found {len(peaks)} peaks at "
              f"{[round(p, 2) for p in peaks]}")
        print(f"    Saved {wav}")
        assert len(peaks) == n, (
            f"voices={n} expected {n} peaks in 420–460 Hz, found {len(peaks)}"
        )
    print("  ✓ PASS: peak counts match voices for all of {2, 4, 8}")


# =============================================================================
# Test 4: poly + unison composition, 300s long-window render
# =============================================================================

RENDER_SECONDS = 300.0  # ≥300s per CLAUDE.md sequenced-opcode rule
RENDER_BPM = 110.0


def test_poly_unison_300s():
    """
    poly() of a unisoned voice — the §4.5 composition example, simplified.
    Render 300s of audio at 110 BPM and check for:

    - No NaN / Inf samples.
    - No sustained clipping (<0.1% of samples at ±1.0). Brief peak alignments
      from 16 constructively-phasing saws at chord transitions are normal;
      sustained clipping is not.
    - Steady-state RMS doesn't drop to <10% of median in any 1-second window
      (catches voice-stealing / chord-transition hiccups, similar to the
      regression test_op_poly.py guards against).
    - DC offset < 0.005.

    Mirrors unison-pad.akk but simplifies the signal chain (drops the filter
    + reverb so the audio level is more predictable). A scalar attenuation
    keeps the 4-chord × 4-poly × 4-unison stack below the int16 ceiling;
    unison-pad.akk gets the same headroom from its `* 0.6 + freeverb * 0.4`
    blend.
    """
    print("\nTest 4: poly + unison 300s long-window render")
    print("=" * 60)

    source = (
        "fn pad_voice(f, g, v, e) -> "
        "saw(f, e.phase) * adsr(g, 0.05, 0.2, 0.7, 0.4) * v\n"
        "fn fat(f, g, v) -> "
        "unison(f, g, v, pad_voice, voices: 4, detune: 0.25, "
        "width: 0.7, phase: 0.5)\n"
        "chord(\"Cmaj7 Am7 Fmaj7 G7\") |> poly(%, fat, 4) "
        "|> % * 0.15 |> out(%)\n"
    )
    wav = OUT_DIR / "poly_unison_300s.wav"
    print(f"  Rendering {RENDER_SECONDS}s at {RENDER_BPM} BPM…")
    render(source, wav, seconds=RENDER_SECONDS, bpm=RENDER_BPM)

    samples, sr = load_wav(wav)
    assert samples.ndim == 2, "Expected stereo WAV"
    mono = samples.mean(axis=1)
    duration = len(mono) / sr
    print(f"  Loaded {len(mono)} samples at {sr} Hz ({duration:.2f}s)")
    assert abs(duration - RENDER_SECONDS) < 1.0, (
        f"Render duration {duration:.2f}s differs from requested "
        f"{RENDER_SECONDS}s by >1s"
    )

    # NaN/Inf check
    assert np.all(np.isfinite(samples)), "WAV contains NaN/Inf samples"

    # Clipping check: brief alignment peaks of 16 oscillators (4 chord × 4 poly
    # × 4 unison) can momentarily kiss the ceiling and that's not a glitch.
    # Sustained clipping (>0.1% of samples at full scale) is.
    peak = float(np.max(np.abs(samples)))
    clipped_frac = float(np.mean(np.abs(samples) >= 0.999))
    print(f"  Peak amplitude: {peak:.4f}  "
          f"clipped@±1.0: {clipped_frac:.4%} of samples")
    assert clipped_frac < 0.001, (
        f"{clipped_frac:.4%} of samples at full scale — sustained "
        f"clipping, not just transient peak alignment"
    )

    # DC offset
    dc = float(np.mean(samples))
    print(f"  DC offset: {dc:+.6f}")
    assert abs(dc) < 0.005, f"DC offset {dc:.4f} exceeds 0.005 tolerance"

    # Per-second RMS — skip 1 s start transient, then check no 1-second window
    # drops to <10% of the median. A drop of that magnitude indicates a chord
    # transition where voices failed to retrigger (the poly bug class).
    skip_samples = sr  # 1 s
    steady = mono[skip_samples:]
    n_full_seconds = len(steady) // sr
    win_rms = np.array([
        float(np.sqrt(np.mean(steady[i * sr:(i + 1) * sr] ** 2)))
        for i in range(n_full_seconds)
    ])
    median_rms = float(np.median(win_rms))
    min_rms = float(win_rms.min())
    min_idx = int(np.argmin(win_rms))
    print(f"  1s-window RMS: median={median_rms:.4f} min={min_rms:.4f} "
          f"(at t≈{min_idx + 1}s, {n_full_seconds} windows)")
    assert median_rms > 1e-3, (
        f"Median RMS {median_rms:.6f} too low — patch may be silent"
    )
    drop_ratio = min_rms / median_rms
    assert drop_ratio >= 0.10, (
        f"1s-window at t≈{min_idx + 1}s dropped to RMS {min_rms:.4f} "
        f"({drop_ratio:.1%} of median) — possible voice-stealing dropout"
    )

    print(f"  Saved {wav} — Listen for hiccups, dropped notes, glitches")
    print(f"  ✓ PASS: 300s renders cleanly, no clipping/dropouts")


# =============================================================================
# Main
# =============================================================================

def main() -> int:
    print("unison Stdlib Function Quality Tests")
    print("=" * 60)
    print(f"Renderer: {NKIDO_CLI}")
    print(f"Output:   {OUT_DIR}")

    if not NKIDO_CLI.exists():
        print(f"\n✗ FAIL: nkido-cli not found at {NKIDO_CLI}")
        print(f"  Build with: cmake --build {REPO_ROOT}/build --target nkido-cli")
        return 2

    failures: list[tuple[str, str]] = []
    for test in [
        test_unison_spectrum_5voice,
        test_unison_voices_1_special_case,
        test_unison_voices_sweep,
        test_poly_unison_300s,
    ]:
        try:
            test()
        except AssertionError as e:
            failures.append((test.__name__, str(e)))
            print(f"  ✗ {test.__name__} FAILED: {e}")
        except Exception as e:
            failures.append((test.__name__, f"{type(e).__name__}: {e}"))
            print(f"  ✗ {test.__name__} ERRORED: {e}")

    print("\n" + "=" * 60)
    if failures:
        print(f"FAILED: {len(failures)} test(s)")
        for name, msg in failures:
            print(f"  ✗ {name}: {msg}")
        return 1
    print("All unison tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
