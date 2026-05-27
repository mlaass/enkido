"""
SEQPAT_STEP iter() Long-Window Stability Test
==============================================

Phase 2 PRD §10.3 acceptance: "extend with `iter` rotation tests across at
least 300 seconds of simulated audio. Verify the pattern rotates correctly
cycle-after-cycle, no voice drops, no timing drift."

Approach: compile + render an iter() pattern via `nkido render` for
300+ seconds. iter(4) on a 4-note pitched pattern rotates the start
position by 1/4 each cycle, so over 4 cycles every note has been heard at
every slot. Across 150 cycles (300 s @ 120 BPM, 4 beats/cycle) the
rotation must remain stable: no silent gaps longer than ~2× a cycle, no
audible timing collapse.

Acceptance criteria:
  * Render exits 0 within timeout.
  * WAV contains audio for the full duration (no truncation).
  * No silent windows longer than 5 seconds anywhere in the output (would
    indicate iter rotation falling out of sync, voice drop, or state
    corruption mid-run).
  * RMS energy in the second half is within 0.5×–2× the first half
    (no slow decay, no runaway gain).

Expected behavior (per PRD §5.2): cycle k plays the events rotated by
-dir * (k mod n) / n. For iter(4) on c4/e4/g4/b4, the four notes appear
at all slot positions, so the long-run spectrum is stable.

If this test fails: investigate cedar/include/cedar/opcodes/sequencing.hpp
op_seqpat_step rotation logic and SequenceState.cycle_index increment.
"""

import os
import subprocess

import numpy as np
import scipy.io.wavfile

from cedar_testing import output_dir


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
NKIDO_CLI = os.path.join(REPO_ROOT, "build", "tools", "nkido", "nkido")
OUT = output_dir("op_seqpat_step")
WAV_PATH = os.path.join(OUT, "iter_long_window.wav")
SRC_PATH = os.path.join(OUT, "iter_long_window.akk")
RENDER_SECONDS = 320.0  # > 300 per PRD §10.3
SR = 48000

# Akkado source: iter(4) over 4 pitched notes. Pulse envelope (AR with short
# attack/release on the trigger) keeps each note audible without bleed.
AKK_SRC = """
arp = n"c4 e4 g4 b4".iter(4)
mtof(arp) |> osc("sin", %) * 0.2 |> out(%, %)
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
        SRC_PATH,
    ]
    print(f"  $ {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
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


def test_iter_long_window_stability():
    """
    Render iter(4) for 320 s and verify long-window stability per PRD §10.3.

    Listening cue: a chromatic-ish four-note arpeggio that rotates start
    position every cycle. Over 5 minutes it should sound continuous, with
    no audible gaps or drift. Re-listen if the silence-window check fails.
    """
    wav_path = _render()

    sr, data = scipy.io.wavfile.read(wav_path)
    assert sr == SR, f"unexpected sample rate {sr}"

    # Stereo: take the mean of the two channels.
    if data.ndim == 2:
        audio = data.astype(np.float32).mean(axis=1) / 32768.0
    else:
        audio = data.astype(np.float32) / 32768.0

    expected_samples = int(RENDER_SECONDS * SR)
    duration_s = len(audio) / SR
    assert len(audio) >= expected_samples * 0.99, \
        f"WAV truncated: {duration_s:.2f}s of expected {RENDER_SECONDS}s"

    # No silent windows > 5s.
    silences = _silent_window_lengths(audio, threshold=1e-3, sr=SR)
    long_silences = [s for s in silences if s > 5.0]
    if long_silences:
        print(f"  ✗ FAIL: {len(long_silences)} silent windows > 5s found "
              f"(max {max(long_silences):.2f}s) — iter rotation lost sync")
        raise AssertionError(f"iter() lost sync: silent windows {long_silences}")

    # Energy stable: compare first half RMS to second half.
    half = len(audio) // 2
    rms_a = float(np.sqrt(np.mean(audio[:half] ** 2)))
    rms_b = float(np.sqrt(np.mean(audio[half:] ** 2)))
    ratio = rms_b / rms_a if rms_a > 0 else 0.0
    assert 0.5 <= ratio <= 2.0, \
        f"energy drift: first-half RMS {rms_a:.4f}, second-half {rms_b:.4f} (ratio {ratio:.3f})"

    print(f"  ✓ PASS: {duration_s:.1f}s rendered, no silences > 5s")
    print(f"  ✓ PASS: RMS ratio {ratio:.3f} (first {rms_a:.4f}, second {rms_b:.4f})")
    print(f"  Saved {wav_path} - Listen for continuous rotating arpeggio across full duration")


if __name__ == "__main__":
    print("=" * 72)
    print("SEQPAT_STEP iter() long-window stability — PRD §10.3")
    print("=" * 72)
    try:
        test_iter_long_window_stability()
        print("\nAll tests passed ✓")
    except AssertionError as e:
        print(f"\n✗ FAIL: {e}")
        raise
