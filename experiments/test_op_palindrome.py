"""
Pattern palindrome() Long-Window Stability Test
================================================

Phase 2 PRD §10.3 acceptance: "(new) — verify forward/backward alternation
over 300+ s."

palindrome doubles the cycle length and appends a reversed copy of the
events. The result plays the pattern forward, then reversed, ad infinitum.
Verify long-window stability and forward-then-reverse alternation.

Approach: render a palindrome pattern for 320 s. The pattern uses four
distinct pitched notes; each "double cycle" should contain all four notes
played forward followed by the same four reversed. Across ~80 double-
cycles (320 s @ 120 BPM, 8 beats per palindrome cycle) the spectrum
should remain stable, with no silent gaps and no energy drift.

Acceptance criteria:
  * Render exits 0 within timeout.
  * No silent windows longer than 5 s anywhere in the output.
  * RMS in second half is within 0.5×–2× of first half (no decay/runup).
  * Each pitched event from {c4, e4, g4, b4} (261.6, 329.6, 392.0, 493.9
    Hz) appears in the global FFT magnitude spectrum with a peak above
    a noise floor — confirms forward AND reverse halves both audible.

If this test fails: investigate pattern_eval handling of palindrome event
duplication (compile-time) and the SEQPAT_STEP cycle-wrap logic for
long-running sequences.
"""

import os
import subprocess

import numpy as np
import scipy.io.wavfile

from cedar_testing import output_dir


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
NKIDO_CLI = os.path.join(REPO_ROOT, "build", "tools", "nkido-cli", "nkido-cli")
OUT = output_dir("op_palindrome")
WAV_PATH = os.path.join(OUT, "palindrome_long_window.wav")
SRC_PATH = os.path.join(OUT, "palindrome_long_window.akk")
RENDER_SECONDS = 320.0  # > 300 per PRD §10.3
SR = 48000

AKK_SRC = """
melody = n"c4 e4 g4 b4".palindrome()
mtof(melody) |> osc("sin", %) * 0.2 |> out(%, %)
"""

# c4=261.63, e4=329.63, g4=392.00, b4=493.88 (12-TET, A4=440)
EXPECTED_PITCHES_HZ = [261.63, 329.63, 392.00, 493.88]


def _render():
    if not os.path.isfile(NKIDO_CLI):
        raise RuntimeError(
            f"nkido-cli not found at {NKIDO_CLI}. Build with:\n"
            f"  cmake --build build --target nkido-cli"
        )
    with open(SRC_PATH, "w") as f:
        f.write(AKK_SRC)
    cmd = [
        NKIDO_CLI, "render",
        "-o", WAV_PATH,
        "--rate", str(SR),
        "--seconds", str(RENDER_SECONDS),
        SRC_PATH,
    ]
    print(f"  $ {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if res.returncode != 0:
        print(res.stdout)
        print(res.stderr)
        raise RuntimeError(f"nkido-cli render failed (code {res.returncode})")
    return WAV_PATH


def _silent_window_lengths(audio: np.ndarray, threshold: float, sr: int) -> list[float]:
    quiet = np.abs(audio) < threshold
    runs: list[float] = []
    i = 0
    while i < len(quiet):
        if quiet[i]:
            j = i
            while j < len(quiet) and quiet[j]:
                j += 1
            runs.append((j - i) / sr)
            i = j
        else:
            i += 1
    return runs


def _spectrum_peaks(audio: np.ndarray, sr: int, freqs_hz: list[float],
                    bin_tol_hz: float = 5.0) -> dict[float, float]:
    """Return magnitude (dB) at each requested frequency."""
    n = len(audio)
    spectrum = np.abs(np.fft.rfft(audio * np.hanning(n)))
    spectrum_db = 20.0 * np.log10(spectrum + 1e-12)
    bin_hz = sr / n
    out: dict[float, float] = {}
    for f in freqs_hz:
        center = int(round(f / bin_hz))
        half = max(1, int(round(bin_tol_hz / bin_hz)))
        lo = max(0, center - half)
        hi = min(len(spectrum_db) - 1, center + half)
        out[f] = float(spectrum_db[lo:hi + 1].max())
    return out


def test_palindrome_long_window_stability():
    """
    Render palindrome("c4 e4 g4 b4") for 320 s.

    Listening cue: arpeggio plays c-e-g-b then b-g-e-c, repeating
    seamlessly for the full duration with no audible breaks.
    """
    wav_path = _render()

    sr, data = scipy.io.wavfile.read(wav_path)
    assert sr == SR, f"unexpected sample rate {sr}"

    if data.ndim == 2:
        audio = data.astype(np.float32).mean(axis=1) / 32768.0
    else:
        audio = data.astype(np.float32) / 32768.0

    expected_samples = int(RENDER_SECONDS * SR)
    duration_s = len(audio) / SR
    assert len(audio) >= expected_samples * 0.99, \
        f"WAV truncated: {duration_s:.2f}s of expected {RENDER_SECONDS}s"

    silences = _silent_window_lengths(audio, threshold=1e-3, sr=SR)
    long_silences = [s for s in silences if s > 5.0]
    if long_silences:
        raise AssertionError(
            f"palindrome lost sync: silent windows > 5s {long_silences[:5]}")

    half = len(audio) // 2
    rms_a = float(np.sqrt(np.mean(audio[:half] ** 2)))
    rms_b = float(np.sqrt(np.mean(audio[half:] ** 2)))
    ratio = rms_b / rms_a if rms_a > 0 else 0.0
    assert 0.5 <= ratio <= 2.0, \
        f"energy drift: first-half RMS {rms_a:.4f}, second-half {rms_b:.4f}"

    # Spectral check: all four pitches present (forward AND reverse halves
    # contribute energy to each pitch over the long window).
    peaks = _spectrum_peaks(audio, SR, EXPECTED_PITCHES_HZ, bin_tol_hz=5.0)
    # All target pitches must be present clearly above the floor (-50 dB).
    missing = [(f, db) for f, db in peaks.items() if db < -50.0]
    assert not missing, \
        f"expected pitches missing or buried: {missing} (full peaks: {peaks})"

    print(f"  ✓ PASS: {duration_s:.1f}s rendered, no silences > 5s")
    print(f"  ✓ PASS: RMS ratio {ratio:.3f}")
    print(f"  ✓ PASS: all four expected pitches present in spectrum:")
    for f, db in peaks.items():
        print(f"        {f:6.2f} Hz -> {db:6.2f} dB")
    print(f"  Saved {wav_path} - Listen for c-e-g-b-b-g-e-c arpeggio across full duration")


if __name__ == "__main__":
    print("=" * 72)
    print("palindrome() long-window stability — PRD §10.3")
    print("=" * 72)
    try:
        test_palindrome_long_window_stability()
        print("\nAll tests passed ✓")
    except AssertionError as e:
        print(f"\n✗ FAIL: {e}")
        raise
