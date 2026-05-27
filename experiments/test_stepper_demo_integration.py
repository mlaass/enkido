"""
Stepper Demo Integration Test
=============================
End-to-end regression guard for `web/static/patches/stepper-demo.akk`.

Compiles + renders the demo via `nkido render` and verifies via STFT
pitch tracking that BOTH voices walk through their notes:

  * Left (melody) — forward stepper, 5 distinct pentatonic notes
  * Right (bass)  — backward stepper, same 5 notes one octave lower

If the demo is silent-stepping (the user-reported bug), each channel will
have only one dominant pitch across the entire render. The test asserts
≥ 4 distinct pitches in the melody and ≥ 2 in the bass over a 6 s render.

This test is a REGRESSION GUARD. It should fail today (because the demo
is broken) and pass once the underlying bug — pinned by
`test_step_pattern.py` and `test_state_set_in_block.py` — is fixed.
"""

import os
import shutil
import subprocess
import numpy as np
import scipy.io.wavfile
from cedar_testing import output_dir


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
DEMO_PATH = os.path.join(REPO_ROOT, "web", "static", "patches", "stepper-demo.akk")
NKIDO_CLI = os.path.join(REPO_ROOT, "build", "tools", "nkido", "nkido")
OUT = output_dir("stepper_demo")
WAV_PATH = os.path.join(OUT, "stepper-demo.wav")
RENDER_SECONDS = 6.0
SR = 48000


def _render():
    """Render the demo with nkido. Returns the WAV path."""
    if not os.path.isfile(NKIDO_CLI):
        raise RuntimeError(
            f"nkido not found at {NKIDO_CLI}. Build it with:\n"
            f"  cmake --build build --target nkido"
        )
    if not os.path.isfile(DEMO_PATH):
        raise RuntimeError(f"Demo not found at {DEMO_PATH}")

    cmd = [
        NKIDO_CLI, "render",
        "-o", WAV_PATH,
        "--rate", str(SR),
        "--seconds", str(RENDER_SECONDS),
        DEMO_PATH,
    ]
    print(f"  $ {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if res.returncode != 0:
        print("  --- nkido stdout ---")
        print(res.stdout)
        print("  --- nkido stderr ---")
        print(res.stderr)
        raise RuntimeError(f"nkido failed with exit code {res.returncode}")
    return WAV_PATH


def _track_pitches(channel: np.ndarray, hop_s: float = 0.05,
                   win_s: float = 0.10, min_hz: float = 40.0,
                   max_hz: float = 2000.0) -> list[float]:
    """
    Per-frame dominant-bin pitch tracker. For each frame, FFT, find the bin
    with maximum magnitude in [min_hz, max_hz], return its frequency.
    """
    win = int(win_s * SR)
    hop = int(hop_s * SR)
    pitches = []
    n_frames = max(0, (len(channel) - win) // hop + 1)
    bin_hz = SR / win
    min_bin = max(1, int(min_hz / bin_hz))
    max_bin = min(win // 2, int(max_hz / bin_hz))
    window = np.hanning(win).astype(np.float32)
    for f in range(n_frames):
        frame = channel[f * hop : f * hop + win] * window
        spec = np.abs(np.fft.rfft(frame))
        if max_bin <= min_bin:
            pitches.append(0.0)
            continue
        peak_bin = min_bin + int(np.argmax(spec[min_bin:max_bin]))
        pitches.append(peak_bin * bin_hz)
    return pitches


def _hz_to_midi(hz: float) -> float:
    if hz <= 0:
        return 0.0
    return 12.0 * np.log2(hz / 440.0) + 69.0


def _quantize_to_semitones(midi_floats: list[float]) -> list[int]:
    """Group near-equal pitches into the same semitone bucket."""
    return sorted({int(round(m)) for m in midi_floats if m > 0})


def test_stepper_demo_walks_both_voices():
    print("\n[integration] stepper-demo.akk renders melody+bass that walk the notes")
    print("-" * 60)

    _render()
    sr, audio = scipy.io.wavfile.read(WAV_PATH)
    if audio.dtype != np.float32:
        audio = audio.astype(np.float32) / np.iinfo(audio.dtype).max
    if audio.ndim == 1:
        # Mono — duplicate for analysis
        left, right = audio, audio
    else:
        left = audio[:, 0]
        right = audio[:, 1]

    print(f"  Loaded {WAV_PATH}: {len(left)} samples = {len(left)/sr:.1f}s @ {sr} Hz")
    print(f"  RMS — left: {np.sqrt(np.mean(left**2)):.4f}  right: {np.sqrt(np.mean(right**2)):.4f}")

    melody_pitches_hz = _track_pitches(left)
    bass_pitches_hz = _track_pitches(right)

    melody_midi = [_hz_to_midi(p) for p in melody_pitches_hz]
    bass_midi = [_hz_to_midi(p) for p in bass_pitches_hz]

    melody_distinct = _quantize_to_semitones(melody_midi)
    bass_distinct = _quantize_to_semitones(bass_midi)

    print(f"  melody distinct semitones over render: {melody_distinct}")
    print(f"  bass   distinct semitones over render: {bass_distinct}")
    print(f"  Saved {WAV_PATH} - listen for two pitched voices walking notes (not a static drone)")

    melody_ok = len(melody_distinct) >= 4
    bass_ok = len(bass_distinct) >= 2

    sym_m = "✓" if melody_ok else "✗"
    sym_b = "✓" if bass_ok else "✗"
    print(f"  {sym_m} melody: {len(melody_distinct)} distinct semitones (need ≥4)")
    print(f"  {sym_b} bass:   {len(bass_distinct)} distinct semitones (need ≥2)")

    if melody_ok and bass_ok:
        print(f"  ✓ PASS: stepper-demo voices walk through pentatonic notes")
        return True
    else:
        print(f"  ✗ FAIL: stepper-demo is silent-stepping — see")
        print(f"          test_step_pattern.py and test_state_set_in_block.py for diagnosis.")
        return False


if __name__ == "__main__":
    print("Stepper Demo Integration Test")
    print("=" * 60)
    ok = test_stepper_demo_walks_both_voices()
    print()
    print("=" * 60)
    if not ok:
        raise SystemExit(1)
    print("PASS — stepper-demo.akk plays correctly.")
