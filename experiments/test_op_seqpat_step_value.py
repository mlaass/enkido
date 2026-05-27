"""
SEQPAT_STEP v"…" Long-Window Stability Test
============================================

PRD prd-patterns-as-scalar-values §10.3 acceptance:
"verify v\"…\"-driven osc modulation produces stepped Hz output matching
the parsed atoms across 300+ seconds of simulated audio."

Approach: render osc("sin", v"<220 440 880 660>") for 320 s via
`nkido render`. Each cycle (4 beats @ 120 BPM = 2 s) the freq buffer
should step through 220, 440, 880, 660 Hz with no mtof applied. Across
160 cycles the output spectrum must remain dominated by exactly those
four bins, with stable RMS energy and no silent windows.

Acceptance criteria:
  * Render exits 0 within timeout.
  * WAV contains the full duration (no truncation).
  * Spectral peaks (averaged across the second half of the run) include
    bins corresponding to 220 / 440 / 880 / 660 Hz, each within ±5 Hz of
    its nominal value.
  * RMS energy in the second half is within 0.5×–2× the first half (no
    drift, no value-buffer corruption).
  * No silent windows longer than 5 seconds (would indicate SEQPAT_STEP
    losing sync or value-buffer reset to 0).

If this test fails: investigate the SEQPAT_STEP value path —
cedar/include/cedar/opcodes/sequencing.hpp and the
SequenceCompiler::compile_atom_event Value branch in
akkado/src/codegen_patterns.cpp (must write atom_data.scalar_value into
event.values[0] verbatim, no mtof).
"""

import os
import subprocess

import numpy as np
import scipy.io.wavfile

from cedar_testing import output_dir


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
NKIDO_CLI = os.path.join(REPO_ROOT, "build", "tools", "nkido", "nkido")
OUT = output_dir("op_seqpat_step_value")
WAV_PATH = os.path.join(OUT, "value_pattern_long_window.wav")
SRC_PATH = os.path.join(OUT, "value_pattern_long_window.akk")
RENDER_SECONDS = 320.0  # > 300 per PRD §10.3
SR = 48000
BPM = 120.0

# 4 raw freq atoms — no mtof. With 4 beats/cycle at 120 BPM the cycle is
# 2.0 s, so over 320 s we get 160 cycles. Output should be dominated by
# exactly four spectral bins.
EXPECTED_FREQS = (220.0, 440.0, 880.0, 660.0)
AKK_SRC = """
osc("sin", v"<220 440 880 660>") * 0.2 |> out(%, %)
"""


def _render():
    if not os.path.isfile(NKIDO_CLI):
        raise RuntimeError(
            f"nkido not found at {NKIDO_CLI}. Build with:\n"
            f"  cmake --build build --target nkido"
        )
    with open(SRC_PATH, "w") as f:
        f.write(AKK_SRC)
    cmd = [
        NKIDO_CLI, "render",
        "-o", WAV_PATH,
        "--rate", str(SR),
        "--seconds", str(RENDER_SECONDS),
        "--bpm", str(BPM),
        SRC_PATH,
    ]
    print(f"  $ {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if res.returncode != 0:
        print(res.stdout)
        print(res.stderr)
        raise RuntimeError(f"nkido render failed (code {res.returncode})")
    return WAV_PATH


def _silent_window_lengths(audio: np.ndarray, threshold: float, sr: int) -> list[float]:
    """Find runs of |audio| < threshold and return their durations in seconds."""
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


def _peak_freqs(audio: np.ndarray, sr: int, n_peaks: int) -> list[float]:
    """Return the n_peaks strongest spectral peaks (in Hz) of `audio`."""
    # Use a Hann window over the whole segment and a magnitude FFT.
    window = np.hanning(len(audio))
    spectrum = np.abs(np.fft.rfft(audio * window))
    freqs = np.fft.rfftfreq(len(audio), 1.0 / sr)

    # Peak picking: take the highest-magnitude bins, then merge anything
    # within a 4-bin neighbourhood so we report distinct peaks.
    order = np.argsort(spectrum)[::-1]
    picked: list[int] = []
    for idx in order:
        if all(abs(int(idx) - p) > 4 for p in picked):
            picked.append(int(idx))
        if len(picked) >= n_peaks:
            break
    return sorted(float(freqs[p]) for p in picked)


def test_value_pattern_long_window_stability():
    """
    Render osc("sin", v"<220 440 880 660>") for 320 s and verify the
    raw scalar atoms reach the freq buffer unchanged across the full run.

    Listening cue: a stable four-tone arpeggio at 220/440/880/660 Hz,
    cycling every 2 s. Over 5+ minutes the timbre must stay constant —
    no slow detune, no bursts of silence, no drift in pitch.
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

    # No silent windows > 5s — SEQPAT_STEP must keep the freq buffer alive.
    silences = _silent_window_lengths(audio, threshold=1e-3, sr=SR)
    long_silences = [s for s in silences if s > 5.0]
    if long_silences:
        print(f"  ✗ FAIL: {len(long_silences)} silent windows > 5s found "
              f"(max {max(long_silences):.2f}s) — value buffer lost")
        raise AssertionError(f"silent windows {long_silences}")

    # Energy stable across halves.
    half = len(audio) // 2
    rms_a = float(np.sqrt(np.mean(audio[:half] ** 2)))
    rms_b = float(np.sqrt(np.mean(audio[half:] ** 2)))
    ratio = rms_b / rms_a if rms_a > 0 else 0.0
    assert 0.5 <= ratio <= 2.0, \
        f"energy drift: first-half RMS {rms_a:.4f}, second-half {rms_b:.4f} (ratio {ratio:.3f})"

    # Spectral check on the second half: four expected peaks must land
    # within ±5 Hz of 220 / 440 / 660 / 880 Hz. We pick the top 6 peaks
    # to give the test some headroom for harmonics or spectral leakage,
    # then require each expected freq to be matched by some peak.
    second_half = audio[half:]
    peaks = _peak_freqs(second_half, SR, n_peaks=6)
    print(f"  spectrum peaks (Hz): {[round(p, 1) for p in peaks]}")

    matched: dict[float, float] = {}
    for nominal in EXPECTED_FREQS:
        nearest = min(peaks, key=lambda p: abs(p - nominal))
        if abs(nearest - nominal) > 5.0:
            raise AssertionError(
                f"expected ~{nominal} Hz, nearest peak {nearest:.1f} Hz "
                f"(off by {nearest - nominal:+.1f} Hz)"
            )
        matched[nominal] = nearest

    print(f"  ✓ PASS: {duration_s:.1f}s rendered, no silences > 5s")
    print(f"  ✓ PASS: RMS ratio {ratio:.3f} (first {rms_a:.4f}, second {rms_b:.4f})")
    print(f"  ✓ PASS: spectral peaks match v\"…\" atoms within ±5 Hz:")
    for nominal, found in matched.items():
        print(f"      {nominal:6.1f} Hz → {found:6.1f} Hz "
              f"(Δ {found - nominal:+.2f})")
    print(f"  Saved {wav_path} - "
          f"Listen for a stable 4-tone arpeggio, no drift over 5+ minutes")


if __name__ == "__main__":
    print("=" * 72)
    print("SEQPAT_STEP v\"…\" long-window stability — PRD §10.3")
    print("=" * 72)
    try:
        test_value_pattern_long_window_stability()
        print("\nAll tests passed ✓")
    except AssertionError as e:
        print(f"\n✗ FAIL: {e}")
        raise
